/* Copyright (C) 2007-2013 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \defgroup sigstate State support
 *
 * It is possible to do matching on reconstructed applicative flow.
 * This is done by this code. It uses the ::Flow structure to store
 * the list of signatures to match on the reconstructed stream.
 *
 * The Flow::de_state is a ::DetectEngineState structure. This is
 * basically a containter for storage item of type ::DeStateStore.
 * They contains an array of ::DeStateStoreItem which store the
 * state of match for an individual signature identified by
 * DeStateStoreItem::sid.
 *
 * The state is constructed by DeStateDetectStartDetection() which
 * also starts the matching. Work is continued by
 * DeStateDetectContinueDetection().
 *
 * Once a transaction has been analysed DeStateRestartDetection()
 * is used to reset the structures.
 *
 * @{
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 *
 * \brief State based signature handling.
 */

#include "suricata-common.h"

#include "decode.h"

#include "detect.h"
#include "detect-engine.h"
#include "detect-parse.h"
#include "detect-engine-state.h"
#include "detect-engine-dcepayload.h"

#include "detect-flowvar.h"

#include "stream-tcp.h"
#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"

#include "app-layer.h"
#include "app-layer-parser.h"
#include "app-layer-protos.h"
#include "app-layer-htp.h"
#include "app-layer-smb.h"
#include "app-layer-dcerpc-common.h"
#include "app-layer-dcerpc.h"
#include "app-layer-dns-common.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"
#include "util-profiling.h"

#include "flow-util.h"

/** convert enum to string */
#define CASE_CODE(E)  case E: return #E

/******** static internal helpers *********/

static inline int StateIsValid(uint16_t alproto, void *alstate)
{
    if (alstate != NULL) {
        if (alproto == ALPROTO_HTTP) {
            HtpState *htp_state = (HtpState *)alstate;
            if (htp_state->conn != NULL) {
                return 1;
            }
        } else {
            return 1;
        }
    }
    return 0;
}

static inline int TxIsLast(uint64_t tx_id, uint64_t total_txs)
{
    if (total_txs - tx_id <= 1)
        return 1;
    return 0;
}

static DeStateStore *DeStateStoreAlloc(void)
{
    DeStateStore *d = SCMalloc(sizeof(DeStateStore));
    if (unlikely(d == NULL))
        return NULL;
    memset(d, 0, sizeof(DeStateStore));

    return d;
}
static DeStateStoreFlowRules *DeStateStoreFlowRulesAlloc(void)
{
    DeStateStoreFlowRules *d = SCMalloc(sizeof(DeStateStoreFlowRules));
    if (unlikely(d == NULL))
        return NULL;
    memset(d, 0, sizeof(DeStateStoreFlowRules));

    return d;
}

static void DeStateSignatureAppend(DetectEngineState *state, Signature *s, uint32_t inspect_flags, uint8_t direction)
{
    int jump = 0;
    int i = 0;
    DetectEngineStateDirection *dir_state = &state->dir_state[direction & STREAM_TOSERVER ? 0 : 1];
    DeStateStore *store = dir_state->head;

    if (store == NULL) {
        store = DeStateStoreAlloc();
        if (store != NULL) {
            dir_state->head = store;
            dir_state->tail = store;
        }
    } else {
        jump = dir_state->cnt / DE_STATE_CHUNK_SIZE;
        for (i = 0; i < jump; i++) {
            store = store->next;
        }
        if (store == NULL) {
            store = DeStateStoreAlloc();
            if (store != NULL) {
                dir_state->tail->next = store;
                dir_state->tail = store;
            }
        }
    }

    if (store == NULL)
        return;

    SigIntId idx = dir_state->cnt++ % DE_STATE_CHUNK_SIZE;
    store->store[idx].sid = s->num;
    store->store[idx].flags = inspect_flags;

    return;
}

static void DeStateFlowRuleAppend(DetectEngineStateFlow *state, Signature *s,
                                   SigMatch *sm, uint32_t inspect_flags,
                                   uint8_t direction)
{
    int jump = 0;
    int i = 0;
    DetectEngineStateDirectionFlow *dir_state = &state->dir_state[direction & STREAM_TOSERVER ? 0 : 1];
    DeStateStoreFlowRules *store = dir_state->head;

    if (store == NULL) {
        store = DeStateStoreFlowRulesAlloc();
        if (store != NULL) {
            dir_state->head = store;
            dir_state->tail = store;
        }
    } else {
        jump = dir_state->cnt / DE_STATE_CHUNK_SIZE;
        for (i = 0; i < jump; i++) {
            store = store->next;
        }
        if (store == NULL) {
            store = DeStateStoreFlowRulesAlloc();
            if (store != NULL) {
                dir_state->tail->next = store;
                dir_state->tail = store;
            }
        }
    }

    if (store == NULL)
        return;

    SigIntId idx = dir_state->cnt++ % DE_STATE_CHUNK_SIZE;
    store->store[idx].sid = s->num;
    store->store[idx].flags = inspect_flags;
    store->store[idx].nm = sm;

    return;
}

static void DeStateStoreStateVersion(Flow *f,
                                     uint16_t alversion, uint8_t direction)
{
    f->detect_alversion[direction & STREAM_TOSERVER ? 0 : 1] = alversion;
}

static void DeStateStoreFileNoMatchCnt(DetectEngineState *de_state, uint16_t file_no_match, uint8_t direction)
{
    de_state->dir_state[direction & STREAM_TOSERVER ? 0 : 1].filestore_cnt += file_no_match;

    return;
}

static int DeStateStoreFilestoreSigsCantMatch(SigGroupHead *sgh, DetectEngineState *de_state, uint8_t direction)
{
    if (de_state->dir_state[direction & STREAM_TOSERVER ? 0 : 1].filestore_cnt == sgh->filestore_cnt)
        return 1;
    else
        return 0;
}

DetectEngineState *DetectEngineStateAlloc(void)
{
    DetectEngineState *d = SCMalloc(sizeof(DetectEngineState));
    if (unlikely(d == NULL))
        return NULL;
    memset(d, 0, sizeof(DetectEngineState));

    return d;
}

DetectEngineStateFlow *DetectEngineStateFlowAlloc(void)
{
    DetectEngineStateFlow *d = SCMalloc(sizeof(DetectEngineStateFlow));
    if (unlikely(d == NULL))
        return NULL;
    memset(d, 0, sizeof(DetectEngineStateFlow));

    return d;
}

void DetectEngineStateFree(DetectEngineState *state)
{
    DeStateStore *store;
    DeStateStore *store_next;
    int i = 0;

    for (i = 0; i < 2; i++) {
        store = state->dir_state[i].head;
        while (store != NULL) {
            store_next = store->next;
            SCFree(store);
            store = store_next;
        }
    }
    SCFree(state);

    return;
}

void DetectEngineStateFlowFree(DetectEngineStateFlow *state)
{
    DeStateStoreFlowRules *store;
    DeStateStoreFlowRules *store_next;
    int i = 0;

    for (i = 0; i < 2; i++) {
        store = state->dir_state[i].head;
        while (store != NULL) {
            store_next = store->next;
            SCFree(store);
            store = store_next;
        }
    }
    SCFree(state);

    return;
}

static int HasStoredSigs(Flow *f, uint8_t flags)
{
    if (f->de_state != NULL && f->de_state->dir_state[flags & STREAM_TOSERVER ? 0 : 1].cnt != 0) {
        SCLogDebug("global sigs present");
        return 1;
    }

    if (AppLayerParserProtocolSupportsTxs(f->proto, f->alproto)) {
        AppProto alproto = f->alproto;
        void *alstate = FlowGetAppState(f);
        if (!StateIsValid(f->alproto, alstate)) {
            return 0;
        }

        uint64_t inspect_tx_id = AppLayerParserGetTransactionInspectId(f->alparser, flags);
        uint64_t total_txs = AppLayerParserGetTxCnt(f->proto, alproto, alstate);

        for ( ; inspect_tx_id < total_txs; inspect_tx_id++) {
            void *inspect_tx = AppLayerParserGetTx(f->proto, alproto, alstate, inspect_tx_id);
            if (inspect_tx != NULL) {
                DetectEngineState *tx_de_state = AppLayerParserGetTxDetectState(f->proto, alproto, inspect_tx);
                if (tx_de_state == NULL) {
                    continue;
                }
                if (tx_de_state->dir_state[flags & STREAM_TOSERVER ? 0 : 1].cnt != 0) {
                    SCLogDebug("tx %u has sigs present", (uint)inspect_tx_id);
                    return 1;
                }
            }
        }
    }
    return 0;
}

/** \brief Check if we need to inspect this state
 *
 *  State needs to be inspected if:
 *   1. state has been updated
 *   2. we already have de_state in progress
 *
 *  \retval 0 no inspectable state
 *  \retval 1 inspectable state
 *  \retval 2 inspectable state, but no update
 */
int DeStateFlowHasInspectableState(Flow *f, AppProto alproto, uint16_t alversion, uint8_t flags)
{
    int r = 0;

    FLOWLOCK_WRLOCK(f);

    if (!(flags & STREAM_EOF) && f->de_state &&
               f->detect_alversion[flags & STREAM_TOSERVER ? 0 : 1] == alversion) {
        SCLogDebug("unchanged state");
        r = 2;
    } else if (HasStoredSigs(f, flags)) {
        r = 1;
    } else {
        r = 0;
    }
    FLOWLOCK_UNLOCK(f);

    return r;
}

static int StoreState(DetectEngineThreadCtx *det_ctx,
        Flow *f, const uint8_t flags, const uint16_t alversion,
        Signature *s, SigMatch *sm, const uint32_t inspect_flags,
        const uint16_t file_no_match)
{
    if (f->de_state == NULL) {
        f->de_state = DetectEngineStateFlowAlloc();
        if (f->de_state == NULL) {
            return 0;
        }
    }

    DeStateFlowRuleAppend(f->de_state, s, sm, inspect_flags, flags);
    DeStateStoreStateVersion(f, alversion, flags);
    return 1;
}

static void StoreStateTxHandleFiles(DetectEngineThreadCtx *det_ctx, Flow *f,
                                    DetectEngineState *destate, const uint8_t flags,
                                    const uint64_t tx_id, const uint16_t file_no_match)
{
    DeStateStoreFileNoMatchCnt(destate, file_no_match, flags);
    if (DeStateStoreFilestoreSigsCantMatch(det_ctx->sgh, destate, flags) == 1) {
        FileDisableStoringForTransaction(f, flags & (STREAM_TOCLIENT | STREAM_TOSERVER), tx_id);
        destate->dir_state[flags & STREAM_TOSERVER ? 0 : 1].flags |= DETECT_ENGINE_STATE_FLAG_FILE_STORE_DISABLED;
    }
}

static void StoreStateTxFileOnly(DetectEngineThreadCtx *det_ctx,
        Flow *f, const uint8_t flags, const uint64_t tx_id, void *tx,
        const uint16_t file_no_match)
{
    if (AppLayerParserSupportsTxDetectState(f->proto, f->alproto)) {
        DetectEngineState *destate = AppLayerParserGetTxDetectState(f->proto, f->alproto, tx);
        if (destate == NULL) {
            destate = DetectEngineStateAlloc();
            if (destate == NULL)
                return;
            if (AppLayerParserSetTxDetectState(f->proto, f->alproto, tx, destate) < 0) {
                DetectEngineStateFree(destate);
                BUG_ON(1);
                return;
            }
            SCLogDebug("destate created for %"PRIu64, tx_id);
        }
        StoreStateTxHandleFiles(det_ctx, f, destate, flags, tx_id, file_no_match);
    }
}

static void StoreStateTx(DetectEngineThreadCtx *det_ctx,
        Flow *f, const uint8_t flags, const uint16_t alversion,
        const uint64_t tx_id, void *tx,
        Signature *s, SigMatch *sm,
        const uint32_t inspect_flags, const uint16_t file_no_match)
{
    if (AppLayerParserSupportsTxDetectState(f->proto, f->alproto)) {
        DetectEngineState *destate = AppLayerParserGetTxDetectState(f->proto, f->alproto, tx);
        if (destate == NULL) {
            destate = DetectEngineStateAlloc();
            if (destate == NULL)
                return;
            if (AppLayerParserSetTxDetectState(f->proto, f->alproto, tx, destate) < 0) {
                DetectEngineStateFree(destate);
                BUG_ON(1);
                return;
            }
            SCLogDebug("destate created for %"PRIu64, tx_id);
        }

        DeStateSignatureAppend(destate, s, inspect_flags, flags);
        DeStateStoreStateVersion(f, alversion, flags);

        StoreStateTxHandleFiles(det_ctx, f, destate, flags, tx_id, file_no_match);
    }
    SCLogDebug("Stored for TX %"PRIu64, tx_id);
}

int DeStateDetectStartDetection(ThreadVars *tv, DetectEngineCtx *de_ctx,
                                DetectEngineThreadCtx *det_ctx,
                                Signature *s, Packet *p, Flow *f, uint8_t flags,
                                AppProto alproto, uint16_t alversion)
{
    SigMatch *sm = NULL;
    uint16_t file_no_match = 0;
    uint32_t inspect_flags = 0;
    uint8_t direction = (flags & STREAM_TOSERVER) ? 0 : 1;
    int alert_cnt = 0;

    FLOWLOCK_WRLOCK(f);
    /* TX based matches (inspect engines) */
    if (AppLayerParserProtocolSupportsTxs(f->proto, alproto)) {
        uint64_t tx_id = 0;
        uint64_t total_txs = 0;

        void *alstate = FlowGetAppState(f);
        if (!StateIsValid(alproto, alstate)) {
            goto end;
        }

        tx_id = AppLayerParserGetTransactionInspectId(f->alparser, flags);
        total_txs = AppLayerParserGetTxCnt(f->proto, alproto, alstate);
        SCLogDebug("total_txs %"PRIu64, total_txs);

        for (; tx_id < total_txs; tx_id++) {
            int total_matches = 0;
            void *tx = AppLayerParserGetTx(f->proto, alproto, alstate, tx_id);
            if (tx == NULL)
                continue;
            det_ctx->tx_id = tx_id;
            det_ctx->tx_id_set = 1;
            DetectEngineAppInspectionEngine *engine = app_inspection_engine[f->protomap][alproto][direction];
            inspect_flags = 0;
            while (engine != NULL) {
                if (s->sm_lists[engine->sm_list] != NULL) {
                    KEYWORD_PROFILING_SET_LIST(det_ctx, engine->sm_list);
                    int match = engine->Callback(tv, de_ctx, det_ctx, s, f,
                                             flags, alstate,
                                             tx, tx_id);
                    if (match == DETECT_ENGINE_INSPECT_SIG_MATCH) {
                        inspect_flags |= engine->inspect_flags;
                        engine = engine->next;
                        total_matches++;
                        continue;
                    } else if (match == DETECT_ENGINE_INSPECT_SIG_CANT_MATCH) {
                        inspect_flags |= DE_STATE_FLAG_SIG_CANT_MATCH;
                        inspect_flags |= engine->inspect_flags;
                    } else if (match == DETECT_ENGINE_INSPECT_SIG_CANT_MATCH_FILESTORE) {
                        inspect_flags |= DE_STATE_FLAG_SIG_CANT_MATCH;
                        inspect_flags |= engine->inspect_flags;
                        file_no_match++;
                    }
                    break;
                }
                engine = engine->next;
            }
            /* all the engines seem to be exhausted at this point.  If we
             * didn't have a match in one of the engines we would have
             * broken off and engine wouldn't be NULL.  Hence the alert. */
            if (engine == NULL && total_matches > 0) {
                if (!(s->flags & SIG_FLAG_NOALERT)) {
                    PacketAlertAppend(det_ctx, s, p, tx_id,
                            PACKET_ALERT_FLAG_STATE_MATCH|PACKET_ALERT_FLAG_TX);
                } else {
                    DetectSignatureApplyActions(p, s);
                }
                alert_cnt = 1;
            }

            /* if this is the last tx in our list, and it's incomplete: then
             * we store the state so that ContinueDetection knows about it */
            int tx_is_done = (AppLayerParserGetStateProgress(f->proto, alproto, tx, flags) >=
                    AppLayerParserGetStateProgressCompletionStatus(f->proto, alproto, flags));

            if ((engine == NULL && total_matches > 0) || (inspect_flags & DE_STATE_FLAG_SIG_CANT_MATCH)) {
                if (!(TxIsLast(tx_id, total_txs)) || !tx_is_done) {
                    if (engine == NULL || inspect_flags & DE_STATE_FLAG_SIG_CANT_MATCH) {
                        inspect_flags |= DE_STATE_FLAG_FULL_INSPECT;
                    }

                    /* store */
                    StoreStateTx(det_ctx, f, flags, alversion, tx_id, tx,
                            s, sm, inspect_flags, file_no_match);
                } else {
                    StoreStateTxFileOnly(det_ctx, f, flags, tx_id, tx, file_no_match);
                }
            }
        } /* for */

    /* DCERPC matches */
    } else if (s->sm_lists[DETECT_SM_LIST_DMATCH] != NULL &&
               (alproto == ALPROTO_DCERPC || alproto == ALPROTO_SMB ||
                alproto == ALPROTO_SMB2))
    {
        void *alstate = FlowGetAppState(f);
        if (alstate == NULL) {
            goto end;
        }

        KEYWORD_PROFILING_SET_LIST(det_ctx, DETECT_SM_LIST_DMATCH);
        if (alproto == ALPROTO_SMB || alproto == ALPROTO_SMB2) {
            SMBState *smb_state = (SMBState *)alstate;
            if (smb_state->dcerpc_present &&
                DetectEngineInspectDcePayload(de_ctx, det_ctx, s, f,
                                              flags, &smb_state->dcerpc) == 1) {
                if (!(s->flags & SIG_FLAG_NOALERT)) {
                    PacketAlertAppend(det_ctx, s, p, 0,
                            PACKET_ALERT_FLAG_STATE_MATCH);
                } else {
                    DetectSignatureApplyActions(p, s);
                }
                alert_cnt = 1;
            }
        } else {
            if (DetectEngineInspectDcePayload(de_ctx, det_ctx, s, f,
                                              flags, alstate) == 1) {
                if (!(s->flags & SIG_FLAG_NOALERT)) {
                    PacketAlertAppend(det_ctx, s, p, 0,
                            PACKET_ALERT_FLAG_STATE_MATCH);
                } else {
                    DetectSignatureApplyActions(p, s);
                }
                alert_cnt = 1;
            }
        }
    }

    /* flow based matches */
    KEYWORD_PROFILING_SET_LIST(det_ctx, DETECT_SM_LIST_AMATCH);
    sm = s->sm_lists[DETECT_SM_LIST_AMATCH];
    if (sm != NULL) {
        void *alstate = FlowGetAppState(f);
        if (alstate == NULL) {
            goto end;
        }

        int match = 0;
        for ( ; sm != NULL; sm = sm->next) {
            if (sigmatch_table[sm->type].AppLayerMatch != NULL) {
                match = 0;
                if (alproto == ALPROTO_SMB || alproto == ALPROTO_SMB2) {
                    SMBState *smb_state = (SMBState *)alstate;
                    if (smb_state->dcerpc_present) {
                        KEYWORD_PROFILING_START;
                        match = sigmatch_table[sm->type].
                            AppLayerMatch(tv, det_ctx, f, flags, &smb_state->dcerpc, s, sm);
                        KEYWORD_PROFILING_END(det_ctx, sm->type, (match == 1));
                    }
                } else {
                    KEYWORD_PROFILING_START;
                    match = sigmatch_table[sm->type].
                        AppLayerMatch(tv, det_ctx, f, flags, alstate, s, sm);
                    KEYWORD_PROFILING_END(det_ctx, sm->type, (match == 1));
                }

                if (match == 0)
                    break;
                if (match == 2) {
                    inspect_flags |= DE_STATE_FLAG_SIG_CANT_MATCH;
                    break;
                }
            }
        }

        if (sm == NULL || inspect_flags & DE_STATE_FLAG_SIG_CANT_MATCH) {
            if (match == 1) {
                if (!(s->flags & SIG_FLAG_NOALERT)) {
                    PacketAlertAppend(det_ctx, s, p, 0,
                            PACKET_ALERT_FLAG_STATE_MATCH);
                } else {
                    DetectSignatureApplyActions(p, s);
                }
                alert_cnt = 1;
            }
            inspect_flags |= DE_STATE_FLAG_FULL_INSPECT;
        }

        StoreState(det_ctx, f, flags, alversion,
                s, sm, inspect_flags, file_no_match);
    }

 end:
    FLOWLOCK_UNLOCK(f);

    det_ctx->tx_id = 0;
    det_ctx->tx_id_set = 0;
    return alert_cnt ? 1:0;
}

static int DoInspectItem(ThreadVars *tv,
    DetectEngineCtx *de_ctx, DetectEngineThreadCtx *det_ctx,
    DeStateStoreItem *item, const uint8_t dir_state_flags,
    Packet *p, Flow *f, AppProto alproto, uint8_t flags,
    const uint64_t inspect_tx_id, const uint64_t total_txs,

    uint16_t *file_no_match, int inprogress, // is current tx in progress?
    const int next_tx_no_progress)                // tx after current is still dormant
{
    Signature *s = de_ctx->sig_array[item->sid];

    /* check if a sig in state 'full inspect' needs to be reconsidered
     * as the result of a new file in the existing tx */
    if (item->flags & DE_STATE_FLAG_FULL_INSPECT) {
        if (item->flags & (DE_STATE_FLAG_FILE_TC_INSPECT|DE_STATE_FLAG_FILE_TS_INSPECT)) {
            if ((flags & STREAM_TOCLIENT) &&
                    (dir_state_flags & DETECT_ENGINE_STATE_FLAG_FILE_TC_NEW))
            {
                item->flags &= ~DE_STATE_FLAG_FILE_TC_INSPECT;
                item->flags &= ~DE_STATE_FLAG_FULL_INSPECT;
            }

            if ((flags & STREAM_TOSERVER) &&
                    (dir_state_flags & DETECT_ENGINE_STATE_FLAG_FILE_TS_NEW))
            {
                item->flags &= ~DE_STATE_FLAG_FILE_TS_INSPECT;
                item->flags &= ~DE_STATE_FLAG_FULL_INSPECT;
            }
        }

        if (item->flags & DE_STATE_FLAG_FULL_INSPECT) {
            if (TxIsLast(inspect_tx_id, total_txs) || inprogress || next_tx_no_progress) {
                det_ctx->de_state_sig_array[item->sid] = DE_STATE_MATCH_NO_NEW_STATE;
            }
            return 0;
        }
    }

    /* check if a sig in state 'cant match' needs to be reconsidered
     * as the result of a new file in the existing tx */
    if (item->flags & DE_STATE_FLAG_SIG_CANT_MATCH) {
        if ((flags & STREAM_TOSERVER) &&
                (item->flags & DE_STATE_FLAG_FILE_TS_INSPECT) &&
                (dir_state_flags & DETECT_ENGINE_STATE_FLAG_FILE_TS_NEW))
        {
            item->flags &= ~DE_STATE_FLAG_FILE_TS_INSPECT;
            item->flags &= ~DE_STATE_FLAG_SIG_CANT_MATCH;
        } else if ((flags & STREAM_TOCLIENT) &&
                (item->flags & DE_STATE_FLAG_FILE_TC_INSPECT) &&
                (dir_state_flags & DETECT_ENGINE_STATE_FLAG_FILE_TC_NEW))
        {
            item->flags &= ~DE_STATE_FLAG_FILE_TC_INSPECT;
            item->flags &= ~DE_STATE_FLAG_SIG_CANT_MATCH;
        } else {
            if (TxIsLast(inspect_tx_id, total_txs) || inprogress || next_tx_no_progress) {
                det_ctx->de_state_sig_array[item->sid] = DE_STATE_MATCH_NO_NEW_STATE;
            }
            return 0;
        }
    }

    uint8_t alert = 0;
    uint32_t inspect_flags = 0;
    int total_matches = 0;

    RULE_PROFILING_START(p);

    void *alstate = FlowGetAppState(f);
    if (!StateIsValid(alproto, alstate)) {
        RULE_PROFILING_END(det_ctx, s, 0, p);
        return -1;
    }

    det_ctx->tx_id = inspect_tx_id;
    det_ctx->tx_id_set = 1;
    DetectEngineAppInspectionEngine *engine = app_inspection_engine[f->protomap][alproto][(flags & STREAM_TOSERVER) ? 0 : 1];
    void *inspect_tx = AppLayerParserGetTx(f->proto, alproto, alstate, inspect_tx_id);
    if (inspect_tx == NULL) {
        RULE_PROFILING_END(det_ctx, s, 0, p);
        return -1;
    }

    while (engine != NULL) {
        if (!(item->flags & engine->inspect_flags) &&
                s->sm_lists[engine->sm_list] != NULL)
        {
            KEYWORD_PROFILING_SET_LIST(det_ctx, engine->sm_list);
            int match = engine->Callback(tv, de_ctx, det_ctx, s, f,
                    flags, alstate, inspect_tx, inspect_tx_id);
            if (match == DETECT_ENGINE_INSPECT_SIG_MATCH) {
                inspect_flags |= engine->inspect_flags;
                engine = engine->next;
                total_matches++;
                continue;
            } else if (match == DETECT_ENGINE_INSPECT_SIG_CANT_MATCH) {
                inspect_flags |= DE_STATE_FLAG_SIG_CANT_MATCH;
                inspect_flags |= engine->inspect_flags;
            } else if (match == DETECT_ENGINE_INSPECT_SIG_CANT_MATCH_FILESTORE) {
                inspect_flags |= DE_STATE_FLAG_SIG_CANT_MATCH;
                inspect_flags |= engine->inspect_flags;
                (*file_no_match)++;
            }
            break;
        }
        engine = engine->next;
    }
    if (total_matches > 0 && (engine == NULL || inspect_flags & DE_STATE_FLAG_SIG_CANT_MATCH)) {
        if (engine == NULL)
            alert = 1;
        inspect_flags |= DE_STATE_FLAG_FULL_INSPECT;
    }

    item->flags |= inspect_flags;
    if (TxIsLast(inspect_tx_id, total_txs)) {
        det_ctx->de_state_sig_array[item->sid] = DE_STATE_MATCH_NO_NEW_STATE;
    }
    RULE_PROFILING_END(det_ctx, s, (alert == 1), p);

    if (alert) {
        det_ctx->flow_locked = 1;
        SigMatchSignaturesRunPostMatch(tv, de_ctx, det_ctx, p, s);
        det_ctx->flow_locked = 0;

        if (!(s->flags & SIG_FLAG_NOALERT)) {
            PacketAlertAppend(det_ctx, s, p, inspect_tx_id,
                    PACKET_ALERT_FLAG_STATE_MATCH|PACKET_ALERT_FLAG_TX);
        } else {
            PACKET_UPDATE_ACTION(p, s->action);
        }
    }

    DetectFlowvarProcessList(det_ctx, f);
    return 1;
}

/** \internal
 *  \brief Continue Detection for a single "flow" rule (AMATCH)
 */
static int DoInspectFlowRule(ThreadVars *tv,
    DetectEngineCtx *de_ctx, DetectEngineThreadCtx *det_ctx,
    DeStateStoreFlowRule *item, const uint8_t dir_state_flags,
    Packet *p, Flow *f, AppProto alproto, uint8_t flags)
{
    /* flag rules that are either full inspected or unable to match
     * in the de_state_sig_array so that prefilter filters them out */
    if (item->flags & (DE_STATE_FLAG_FULL_INSPECT|DE_STATE_FLAG_SIG_CANT_MATCH)) {
        det_ctx->de_state_sig_array[item->sid] = DE_STATE_MATCH_NO_NEW_STATE;
        return 0;
    }

    uint8_t alert = 0;
    uint32_t inspect_flags = 0;
    int total_matches = 0;
    SigMatch *sm = NULL;
    Signature *s = de_ctx->sig_array[item->sid];

    RULE_PROFILING_START(p);

    KEYWORD_PROFILING_SET_LIST(det_ctx, DETECT_SM_LIST_AMATCH);
    if (item->nm != NULL) {
        void *alstate = FlowGetAppState(f);
        if (alstate == NULL) {
            RULE_PROFILING_END(det_ctx, s, 0 /* no match */, p);
            return -1;
        }

        for (sm = item->nm; sm != NULL; sm = sm->next) {
            if (sigmatch_table[sm->type].AppLayerMatch != NULL)
            {
                int match = 0;
                if (alproto == ALPROTO_SMB || alproto == ALPROTO_SMB2) {
                    SMBState *smb_state = (SMBState *)alstate;
                    if (smb_state->dcerpc_present) {
                        KEYWORD_PROFILING_START;
                        match = sigmatch_table[sm->type].
                            AppLayerMatch(tv, det_ctx, f, flags, &smb_state->dcerpc, s, sm);
                        KEYWORD_PROFILING_END(det_ctx, sm->type, (match == 1));
                    }
                } else {
                    KEYWORD_PROFILING_START;
                    match = sigmatch_table[sm->type].
                        AppLayerMatch(tv, det_ctx, f, flags, alstate, s, sm);
                    KEYWORD_PROFILING_END(det_ctx, sm->type, (match == 1));
                }

                if (match == 0)
                    break;
                else if (match == 2)
                    inspect_flags |= DE_STATE_FLAG_SIG_CANT_MATCH;
                else if (match == 1)
                    total_matches++;
            }
        }
    }

    if (s->sm_lists[DETECT_SM_LIST_AMATCH] != NULL) {
        if (total_matches > 0 && (sm == NULL || inspect_flags & DE_STATE_FLAG_SIG_CANT_MATCH)) {
            if (sm == NULL)
                alert = 1;
            inspect_flags |= DE_STATE_FLAG_FULL_INSPECT;
        }
        /* prevent the rule loop from reinspecting this rule */
        det_ctx->de_state_sig_array[item->sid] = DE_STATE_MATCH_NO_NEW_STATE;
    }
    RULE_PROFILING_END(det_ctx, s, (alert == 1), p);

    /* store the progress in the state */
    item->flags |= inspect_flags;
    item->nm = sm;

    if (alert) {
        det_ctx->flow_locked = 1;
        SigMatchSignaturesRunPostMatch(tv, de_ctx, det_ctx, p, s);
        det_ctx->flow_locked = 0;

        if (!(s->flags & SIG_FLAG_NOALERT)) {
            PacketAlertAppend(det_ctx, s, p, 0,
                    PACKET_ALERT_FLAG_STATE_MATCH);
        } else {
            DetectSignatureApplyActions(p, s);
        }
    }

    DetectFlowvarProcessList(det_ctx, f);
    return 1;
}

void DeStateDetectContinueDetection(ThreadVars *tv, DetectEngineCtx *de_ctx,
                                    DetectEngineThreadCtx *det_ctx,
                                    Packet *p, Flow *f, uint8_t flags,
                                    AppProto alproto, uint16_t alversion)
{
    uint16_t file_no_match = 0;
    SigIntId store_cnt = 0;
    SigIntId state_cnt = 0;
    uint64_t inspect_tx_id = 0;
    uint64_t total_txs = 0;
    uint8_t direction = (flags & STREAM_TOSERVER) ? 0 : 1;

    FLOWLOCK_WRLOCK(f);

    SCLogDebug("starting continue detection for packet %"PRIu64, p->pcap_cnt);

    if (AppLayerParserProtocolSupportsTxs(f->proto, alproto)) {
        void *alstate = FlowGetAppState(f);
        if (!StateIsValid(alproto, alstate)) {
            FLOWLOCK_UNLOCK(f);
            return;
        }

        inspect_tx_id = AppLayerParserGetTransactionInspectId(f->alparser, flags);
        total_txs = AppLayerParserGetTxCnt(f->proto, alproto, alstate);

        for ( ; inspect_tx_id < total_txs; inspect_tx_id++) {
            int inspect_tx_inprogress = 0;
            int next_tx_no_progress = 0;
            void *inspect_tx = AppLayerParserGetTx(f->proto, alproto, alstate, inspect_tx_id);
            if (inspect_tx != NULL) {
                int a = AppLayerParserGetStateProgress(f->proto, alproto, inspect_tx, flags);
                int b = AppLayerParserGetStateProgressCompletionStatus(f->proto, alproto, flags);
                if (a < b) {
                    inspect_tx_inprogress = 1;
                }
                SCLogDebug("tx %"PRIu64" (%"PRIu64") => %s", inspect_tx_id, total_txs,
                        inspect_tx_inprogress ? "in progress" : "done");

                DetectEngineState *tx_de_state = AppLayerParserGetTxDetectState(f->proto, alproto, inspect_tx);
                if (tx_de_state == NULL) {
                    SCLogDebug("NO STATE tx %"PRIu64" (%"PRIu64")", inspect_tx_id, total_txs);
                    continue;
                }
                DetectEngineStateDirection *tx_dir_state = &tx_de_state->dir_state[direction];
                DeStateStore *tx_store = tx_dir_state->head;

                /* see if we need to consider the next tx in our decision to add
                 * a sig to the 'no inspect array'. */
                if (!TxIsLast(inspect_tx_id, total_txs)) {
                    void *next_inspect_tx = AppLayerParserGetTx(f->proto, alproto, alstate, inspect_tx_id+1);
                    if (next_inspect_tx != NULL) {
                        int c = AppLayerParserGetStateProgress(f->proto, alproto, next_inspect_tx, flags);
                        if (c == 0) {
                            next_tx_no_progress = 1;
                        }
                    }
                }

                /* Loop through stored 'items' (stateful rules) and inspect them */
                state_cnt = 0;
                for (; tx_store != NULL; tx_store = tx_store->next) {
                    SCLogDebug("tx_store %p", tx_store);
                    for (store_cnt = 0;
                            store_cnt < DE_STATE_CHUNK_SIZE && state_cnt < tx_dir_state->cnt;
                            store_cnt++, state_cnt++)
                    {
                        DeStateStoreItem *item = &tx_store->store[store_cnt];
                        int r = DoInspectItem(tv, de_ctx, det_ctx,
                                item, tx_dir_state->flags,
                                p, f, alproto, flags,
                                inspect_tx_id, total_txs,
                                &file_no_match, inspect_tx_inprogress, next_tx_no_progress);
                        if (r < 0) {
                            SCLogDebug("failed");
                            goto end;
                        }
                    }
                }
            }
            /* if the current tx is in progress, we won't advance to any newer
             * tx' just yet. */
            if (inspect_tx_inprogress) {
                SCLogDebug("break out");
                break;
            }
        }
    }

    /* continue on flow based state rules (AMATCH) */
    if (f->de_state != NULL) {
        DetectEngineStateDirectionFlow *dir_state = &f->de_state->dir_state[direction];
        DeStateStoreFlowRules *store = dir_state->head;
        /* Loop through stored 'items' (stateful rules) and inspect them */
        for (; store != NULL; store = store->next) {
            for (store_cnt = 0;
                    store_cnt < DE_STATE_CHUNK_SIZE && state_cnt < dir_state->cnt;
                    store_cnt++, state_cnt++)
            {
                DeStateStoreFlowRule *rule = &store->store[store_cnt];

                int r = DoInspectFlowRule(tv, de_ctx, det_ctx,
                        rule, dir_state->flags,
                        p, f, alproto, flags);
                if (r < 0) {
                    goto end;
                }
            }
        }
        DeStateStoreStateVersion(f, alversion, flags);
    }

end:
    FLOWLOCK_UNLOCK(f);
    det_ctx->tx_id = 0;
    det_ctx->tx_id_set = 0;
    return;
}
/** \brief update flow's inspection id's
 *
 *  \note it is possible that f->alstate, f->alparser are NULL */
void DeStateUpdateInspectTransactionId(Flow *f, uint8_t direction)
{
    FLOWLOCK_WRLOCK(f);
    if (f->alparser && f->alstate) {
        AppLayerParserSetTransactionInspectId(f->alparser, f->proto, f->alproto, f->alstate, direction);
    }
    FLOWLOCK_UNLOCK(f);

    return;
}

void DetectEngineStateReset(DetectEngineStateFlow *state, uint8_t direction)
{
    if (state != NULL) {
        if (direction & STREAM_TOSERVER) {
            state->dir_state[0].cnt = 0;
            state->dir_state[0].flags = 0;
        }
        if (direction & STREAM_TOCLIENT) {
            state->dir_state[1].cnt = 0;
            state->dir_state[1].flags = 0;
        }
    }

    return;
}

/** \brief Reset de state for active tx'
 *  To be used on detect engine reload.
 *  \param f write LOCKED flow
 */
void DetectEngineStateResetTxs(Flow *f)
{
    if (AppLayerParserProtocolSupportsTxs(f->proto, f->alproto)) {
        void *alstate = FlowGetAppState(f);
        if (!StateIsValid(f->alproto, alstate)) {
            return;
        }

        uint64_t inspect_ts = AppLayerParserGetTransactionInspectId(f->alparser, STREAM_TOCLIENT);
        uint64_t inspect_tc = AppLayerParserGetTransactionInspectId(f->alparser, STREAM_TOSERVER);

        uint64_t inspect_tx_id = MIN(inspect_ts, inspect_tc);

        uint64_t total_txs = AppLayerParserGetTxCnt(f->proto, f->alproto, alstate);

        for ( ; inspect_tx_id < total_txs; inspect_tx_id++) {
            void *inspect_tx = AppLayerParserGetTx(f->proto, f->alproto, alstate, inspect_tx_id);
            if (inspect_tx != NULL) {
                DetectEngineState *tx_de_state = AppLayerParserGetTxDetectState(f->proto, f->alproto, inspect_tx);
                if (tx_de_state == NULL) {
                    continue;
                }

                tx_de_state->dir_state[0].cnt = 0;
                tx_de_state->dir_state[0].filestore_cnt = 0;
                tx_de_state->dir_state[0].flags = 0;

                tx_de_state->dir_state[1].cnt = 0;
                tx_de_state->dir_state[1].filestore_cnt = 0;
                tx_de_state->dir_state[1].flags = 0;
            }
        }
    }
}

/** \brief get string for match enum */
const char *DeStateMatchResultToString(DeStateMatchResult res)
{
    switch (res) {
        CASE_CODE (DE_STATE_MATCH_NO_NEW_STATE);
        CASE_CODE (DE_STATE_MATCH_HAS_NEW_STATE);
    }

    return NULL;
}

/*********Unittests*********/

#ifdef UNITTESTS
#include "flow-util.h"

static int DeStateTest01(void)
{
    SCLogDebug("sizeof(DetectEngineState)\t\t%"PRIuMAX,
            (uintmax_t)sizeof(DetectEngineState));
    SCLogDebug("sizeof(DeStateStore)\t\t\t%"PRIuMAX,
            (uintmax_t)sizeof(DeStateStore));
    SCLogDebug("sizeof(DeStateStoreItem)\t\t%"PRIuMAX"",
            (uintmax_t)sizeof(DeStateStoreItem));

    return 1;
}

static int DeStateTest02(void)
{
    int result = 0;

    DetectEngineState *state = DetectEngineStateAlloc();
    if (state == NULL) {
        printf("d == NULL: ");
        goto end;
    }

    Signature s;
    memset(&s, 0x00, sizeof(s));

    uint8_t direction = STREAM_TOSERVER;

    s.num = 0;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 11;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 22;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 33;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 44;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 55;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 66;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 77;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 88;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 99;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 100;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 111;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 122;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 133;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 144;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 155;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 166;
    DeStateSignatureAppend(state, &s, 0, direction);

    if (state->dir_state[direction & STREAM_TOSERVER ? 0 : 1].head == NULL) {
        goto end;
    }

    if (state->dir_state[direction & STREAM_TOSERVER ? 0 : 1].head->store[1].sid != 11) {
        goto end;
    }

    if (state->dir_state[direction & STREAM_TOSERVER ? 0 : 1].head->next == NULL) {
        goto end;
    }

    if (state->dir_state[direction & STREAM_TOSERVER ? 0 : 1].head->store[14].sid != 144) {
        goto end;
    }

    if (state->dir_state[direction & STREAM_TOSERVER ? 0 : 1].head->next->store[0].sid != 155) {
        goto end;
    }

    if (state->dir_state[direction & STREAM_TOSERVER ? 0 : 1].head->next->store[1].sid != 166) {
        goto end;
    }

    result = 1;
end:
    if (state != NULL) {
        DetectEngineStateFree(state);
    }
    return result;
}

static int DeStateTest03(void)
{
    int result = 0;

    DetectEngineState *state = DetectEngineStateAlloc();
    if (state == NULL) {
        printf("d == NULL: ");
        goto end;
    }

    Signature s;
    memset(&s, 0x00, sizeof(s));

    uint8_t direction = STREAM_TOSERVER;

    s.num = 11;
    DeStateSignatureAppend(state, &s, 0, direction);
    s.num = 22;
    DeStateSignatureAppend(state, &s, DE_STATE_FLAG_URI_INSPECT, direction);

    if (state->dir_state[direction & STREAM_TOSERVER ? 0 : 1].head == NULL) {
        goto end;
    }

    if (state->dir_state[direction & STREAM_TOSERVER ? 0 : 1].head->store[0].sid != 11) {
        goto end;
    }

    if (state->dir_state[direction & STREAM_TOSERVER ? 0 : 1].head->store[0].flags & DE_STATE_FLAG_URI_INSPECT) {
        goto end;
    }

    if (state->dir_state[direction & STREAM_TOSERVER ? 0 : 1].head->store[1].sid != 22) {
        goto end;
    }

    if (!(state->dir_state[direction & STREAM_TOSERVER ? 0 : 1].head->store[1].flags & DE_STATE_FLAG_URI_INSPECT)) {
        goto end;
    }

    result = 1;
end:
    if (state != NULL) {
        DetectEngineStateFree(state);
    }
    return result;
}

static int DeStateSigTest01(void)
{
    int result = 0;
    Signature *s = NULL;
    DetectEngineThreadCtx *det_ctx = NULL;
    ThreadVars th_v;
    Flow f;
    TcpSession ssn;
    Packet *p = NULL;
    uint8_t httpbuf1[] = "POST / HTTP/1.0\r\n";
    uint8_t httpbuf2[] = "User-Agent: Mozilla/1.0\r\n";
    uint8_t httpbuf3[] = "Cookie: dummy\r\nContent-Length: 10\r\n\r\n";
    uint8_t httpbuf4[] = "Http Body!";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */
    uint32_t httplen4 = sizeof(httpbuf4) - 1; /* minus the \0 */
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any (content:\"POST\"; http_method; content:\"dummy\"; http_cookie; sid:1; rev:1;)");
    if (s == NULL) {
        printf("sig parse failed: ");
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (PacketAlertCheck(p, 1)) {
        printf("sig 1 alerted: ");
        goto end;
    }
    p->alerts.cnt = 0;

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf2, httplen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (PacketAlertCheck(p, 1)) {
        printf("sig 1 alerted (2): ");
        goto end;
    }
    p->alerts.cnt = 0;

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf3, httplen3);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (!(PacketAlertCheck(p, 1))) {
        printf("sig 1 didn't alert: ");
        goto end;
    }
    p->alerts.cnt = 0;

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf4, httplen4);
    if (r != 0) {
        printf("toserver chunk 4 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (PacketAlertCheck(p, 1)) {
        printf("signature matched, but shouldn't have: ");
        goto end;
    }
    p->alerts.cnt = 0;

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (http_state != NULL) {
        HTPStateFree(http_state);
    }
    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    return result;
}

/** \test multiple pipelined http transactions */
static int DeStateSigTest02(void)
{
    int result = 0;
    Signature *s = NULL;
    DetectEngineThreadCtx *det_ctx = NULL;
    ThreadVars th_v;
    Flow f;
    TcpSession ssn;
    Packet *p = NULL;
    uint8_t httpbuf1[] = "POST / HTTP/1.1\r\n";
    uint8_t httpbuf2[] = "User-Agent: Mozilla/1.0\r\nContent-Length: 10\r\n";
    uint8_t httpbuf3[] = "Cookie: dummy\r\n\r\n";
    uint8_t httpbuf4[] = "Http Body!";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */
    uint32_t httplen3 = sizeof(httpbuf3) - 1; /* minus the \0 */
    uint32_t httplen4 = sizeof(httpbuf4) - 1; /* minus the \0 */
    uint8_t httpbuf5[] = "GET /?var=val HTTP/1.1\r\n";
    uint8_t httpbuf6[] = "User-Agent: Firefox/1.0\r\n";
    uint8_t httpbuf7[] = "Cookie: dummy2\r\nContent-Length: 10\r\n\r\nHttp Body!";
    uint32_t httplen5 = sizeof(httpbuf5) - 1; /* minus the \0 */
    uint32_t httplen6 = sizeof(httpbuf6) - 1; /* minus the \0 */
    uint32_t httplen7 = sizeof(httpbuf7) - 1; /* minus the \0 */
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (content:\"POST\"; http_method; content:\"Mozilla\"; http_header; content:\"dummy\"; http_cookie; sid:1; rev:1;)");
    if (s == NULL) {
        printf("sig parse failed: ");
        goto end;
    }
    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (content:\"GET\"; http_method; content:\"Firefox\"; http_header; content:\"dummy2\"; http_cookie; sid:2; rev:1;)");
    if (s == NULL) {
        printf("sig2 parse failed: ");
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SCMutexLock(&f.m);
    int r = AppLayerParserParse(alp_tctx, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (PacketAlertCheck(p, 1)) {
        printf("sig 1 alerted: ");
        goto end;
    }
    p->alerts.cnt = 0;

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf2, httplen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (PacketAlertCheck(p, 1)) {
        printf("sig 1 alerted (2): ");
        goto end;
    }
    p->alerts.cnt = 0;

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf3, httplen3);
    if (r != 0) {
        printf("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (!(PacketAlertCheck(p, 1))) {
        printf("sig 1 didn't alert: ");
        goto end;
    }
    p->alerts.cnt = 0;

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf4, httplen4);
    if (r != 0) {
        printf("toserver chunk 4 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (PacketAlertCheck(p, 1)) {
        printf("signature matched, but shouldn't have: ");
        goto end;
    }
    p->alerts.cnt = 0;

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf5, httplen5);
    if (r != 0) {
        printf("toserver chunk 5 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (PacketAlertCheck(p, 1)) {
        printf("sig 1 alerted (5): ");
        goto end;
    }
    p->alerts.cnt = 0;

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf6, httplen6);
    if (r != 0) {
        printf("toserver chunk 6 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if ((PacketAlertCheck(p, 1)) || (PacketAlertCheck(p, 2))) {
        printf("sig 1 alerted (request 2, chunk 6): ");
        goto end;
    }
    p->alerts.cnt = 0;

    SCLogDebug("sending data chunk 7");

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf7, httplen7);
    if (r != 0) {
        printf("toserver chunk 7 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);
    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (!(PacketAlertCheck(p, 2))) {
        printf("signature 2 didn't match, but should have: ");
        goto end;
    }
    p->alerts.cnt = 0;

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePacket(p);
    return result;
}

static int DeStateSigTest03(void)
{
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 215\r\n"
                         "\r\n"
                         "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n"
                         "filecontent\r\n"
                         "-----------------------------277531038314945--";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    ThreadVars th_v;
    TcpSession ssn;
    int result = 0;
    Flow *f = NULL;
    Packet *p = NULL;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&ssn, 0, sizeof(ssn));

    DetectEngineThreadCtx *det_ctx = NULL;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    Signature *s = DetectEngineAppendSig(de_ctx, "alert http any any -> any any (content:\"POST\"; http_method; content:\"upload.cgi\"; http_uri; filestore; sid:1; rev:1;)");
    if (s == NULL) {
        printf("sig parse failed: ");
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    if (f == NULL)
        goto end;
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);
    if (p == NULL)
        goto end;

    p->flow = f;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f->m);
    int r = AppLayerParserParse(alp_tctx, f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_START|STREAM_EOF, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        SCMutexUnlock(&f->m);
        goto end;
    }
    SCMutexUnlock(&f->m);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (!(PacketAlertCheck(p, 1))) {
        printf("sig 1 didn't alert: ");
        goto end;
    }

    http_state = f->alstate;
    if (http_state == NULL) {
        printf("no http state: ");
        result = 0;
        goto end;
    }

    if (http_state->files_ts == NULL) {
        printf("no files in state: ");
        goto end;
    }

    SCMutexLock(&f->m);
    FileContainer *files = AppLayerParserGetFiles(p->flow->proto, p->flow->alproto,
                                                  p->flow->alstate, STREAM_TOSERVER);
    if (files == NULL) {
        printf("no stored files: ");
        SCMutexUnlock(&f->m);
        goto end;
    }
    SCMutexUnlock(&f->m);

    File *file = files->head;
    if (file == NULL) {
        printf("no file: ");
        goto end;
    }

    if (!(file->flags & FILE_STORE)) {
        printf("file is set to store, but sig didn't match: ");
        goto end;
    }

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    UTHFreeFlow(f);

    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }
    StreamTcpFreeConfig(TRUE);
    return result;
}

static int DeStateSigTest04(void)
{
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 215\r\n"
                         "\r\n"
                         "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n"
                         "filecontent\r\n"
                         "-----------------------------277531038314945--";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    ThreadVars th_v;
    TcpSession ssn;
    int result = 0;
    Flow *f = NULL;
    Packet *p = NULL;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&ssn, 0, sizeof(ssn));

    DetectEngineThreadCtx *det_ctx = NULL;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    Signature *s = DetectEngineAppendSig(de_ctx, "alert http any any -> any any (content:\"GET\"; http_method; content:\"upload.cgi\"; http_uri; filestore; sid:1; rev:1;)");
    if (s == NULL) {
        printf("sig parse failed: ");
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    if (f == NULL)
        goto end;
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);
    if (p == NULL)
        goto end;

    p->flow = f;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f->m);
    int r = AppLayerParserParse(alp_tctx, f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_START|STREAM_EOF, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        SCMutexUnlock(&f->m);
        goto end;
    }
    SCMutexUnlock(&f->m);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (PacketAlertCheck(p, 1)) {
        printf("sig 1 alerted: ");
        goto end;
    }

    http_state = f->alstate;
    if (http_state == NULL) {
        printf("no http state: ");
        result = 0;
        goto end;
    }

    if (http_state->files_ts == NULL) {
        printf("no files in state: ");
        goto end;
    }

    SCMutexLock(&f->m);
    FileContainer *files = AppLayerParserGetFiles(p->flow->proto, p->flow->alproto,
                                                  p->flow->alstate, STREAM_TOSERVER);
    if (files == NULL) {
        printf("no stored files: ");
        SCMutexUnlock(&f->m);
        goto end;
    }
    SCMutexUnlock(&f->m);

    File *file = files->head;
    if (file == NULL) {
        printf("no file: ");
        goto end;
    }

    if (file->flags & FILE_STORE) {
        printf("file is set to store, but sig didn't match: ");
        goto end;
    }

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    UTHFreeFlow(f);

    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }
    StreamTcpFreeConfig(TRUE);
    return result;
}

static int DeStateSigTest05(void)
{
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 215\r\n"
                         "\r\n"
                         "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n"
                         "filecontent\r\n"
                         "-----------------------------277531038314945--";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    ThreadVars th_v;
    TcpSession ssn;
    int result = 0;
    Flow *f = NULL;
    Packet *p = NULL;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&ssn, 0, sizeof(ssn));

    DetectEngineThreadCtx *det_ctx = NULL;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    Signature *s = DetectEngineAppendSig(de_ctx, "alert http any any -> any any (content:\"GET\"; http_method; content:\"upload.cgi\"; http_uri; filename:\"nomatch\"; sid:1; rev:1;)");
    if (s == NULL) {
        printf("sig parse failed: ");
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    if (f == NULL)
        goto end;
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);
    if (p == NULL)
        goto end;

    p->flow = f;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f->m);
    int r = AppLayerParserParse(alp_tctx, f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_START|STREAM_EOF, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        SCMutexUnlock(&f->m);
        goto end;
    }
    SCMutexUnlock(&f->m);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (PacketAlertCheck(p, 1)) {
        printf("sig 1 alerted: ");
        goto end;
    }

    http_state = f->alstate;
    if (http_state == NULL) {
        printf("no http state: ");
        result = 0;
        goto end;
    }

    if (http_state->files_ts == NULL) {
        printf("no files in state: ");
        goto end;
    }

    SCMutexLock(&f->m);
    FileContainer *files = AppLayerParserGetFiles(p->flow->proto, p->flow->alproto,
                                                  p->flow->alstate, STREAM_TOSERVER);
    if (files == NULL) {
        printf("no stored files: ");
        SCMutexUnlock(&f->m);
        goto end;
    }
    SCMutexUnlock(&f->m);

    File *file = files->head;
    if (file == NULL) {
        printf("no file: ");
        goto end;
    }

    if (!(file->flags & FILE_NOSTORE)) {
        printf("file is not set to \"no store\": ");
        goto end;
    }

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    UTHFreeFlow(f);

    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }
    StreamTcpFreeConfig(TRUE);
    return result;
}

static int DeStateSigTest06(void)
{
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 215\r\n"
                         "\r\n"
                         "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n"
                         "filecontent\r\n"
                         "-----------------------------277531038314945--";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    ThreadVars th_v;
    TcpSession ssn;
    int result = 0;
    Flow *f = NULL;
    Packet *p = NULL;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&ssn, 0, sizeof(ssn));

    DetectEngineThreadCtx *det_ctx = NULL;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    Signature *s = DetectEngineAppendSig(de_ctx, "alert http any any -> any any (content:\"POST\"; http_method; content:\"upload.cgi\"; http_uri; filename:\"nomatch\"; filestore; sid:1; rev:1;)");
    if (s == NULL) {
        printf("sig parse failed: ");
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    if (f == NULL)
        goto end;
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);
    if (p == NULL)
        goto end;

    p->flow = f;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;

    StreamTcpInitConfig(TRUE);

    SCMutexLock(&f->m);
    int r = AppLayerParserParse(alp_tctx, f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_START|STREAM_EOF, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        SCMutexUnlock(&f->m);
        goto end;
    }
    SCMutexUnlock(&f->m);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (PacketAlertCheck(p, 1)) {
        printf("sig 1 alerted: ");
        goto end;
    }

    http_state = f->alstate;
    if (http_state == NULL) {
        printf("no http state: ");
        result = 0;
        goto end;
    }

    if (http_state->files_ts == NULL) {
        printf("no files in state: ");
        goto end;
    }

    SCMutexLock(&f->m);
    FileContainer *files = AppLayerParserGetFiles(p->flow->proto, p->flow->alproto,
                                                  p->flow->alstate, STREAM_TOSERVER);
    if (files == NULL) {
        printf("no stored files: ");
        SCMutexUnlock(&f->m);
        goto end;
    }
    SCMutexUnlock(&f->m);

    File *file = files->head;
    if (file == NULL) {
        printf("no file: ");
        goto end;
    }

    if (!(file->flags & FILE_NOSTORE)) {
        printf("file is not set to \"no store\": ");
        goto end;
    }

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    UTHFreeFlow(f);

    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }
    StreamTcpFreeConfig(TRUE);
    return result;
}

static int DeStateSigTest07(void)
{
    uint8_t httpbuf1[] = "POST /upload.cgi HTTP/1.1\r\n"
                         "Host: www.server.lan\r\n"
                         "Content-Type: multipart/form-data; boundary=---------------------------277531038314945\r\n"
                         "Content-Length: 215\r\n"
                         "\r\n"
                         "-----------------------------277531038314945\r\n"
                         "Content-Disposition: form-data; name=\"uploadfile_0\"; filename=\"somepicture1.jpg\"\r\n"
                         "Content-Type: image/jpeg\r\n"
                         "\r\n";

    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint8_t httpbuf2[] = "filecontent\r\n"
                         "-----------------------------277531038314945--";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */
    ThreadVars th_v;
    TcpSession ssn;
    int result = 0;
    Flow *f = NULL;
    Packet *p = NULL;
    HtpState *http_state = NULL;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&ssn, 0, sizeof(ssn));

    DetectEngineThreadCtx *det_ctx = NULL;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    Signature *s = DetectEngineAppendSig(de_ctx, "alert http any any -> any any (content:\"GET\"; http_method; content:\"upload.cgi\"; http_uri; filestore; sid:1; rev:1;)");
    if (s == NULL) {
        printf("sig parse failed: ");
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    f = UTHBuildFlow(AF_INET, "1.2.3.4", "1.2.3.5", 1024, 80);
    if (f == NULL)
        goto end;
    f->protoctx = &ssn;
    f->proto = IPPROTO_TCP;
    f->alproto = ALPROTO_HTTP;

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);
    if (p == NULL)
        goto end;

    p->flow = f;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;

    StreamTcpInitConfig(TRUE);

    SCLogDebug("\n>>>> processing chunk 1 <<<<\n");
    SCMutexLock(&f->m);
    int r = AppLayerParserParse(alp_tctx, f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_START, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        SCMutexUnlock(&f->m);
        goto end;
    }
    SCMutexUnlock(&f->m);

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (PacketAlertCheck(p, 1)) {
        printf("sig 1 alerted: ");
        goto end;
    }

    SCLogDebug("\n>>>> processing chunk 2 size %u <<<<\n", httplen2);
    SCMutexLock(&f->m);
    r = AppLayerParserParse(alp_tctx, f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_EOF, httpbuf2, httplen2);
    if (r != 0) {
        printf("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        SCMutexUnlock(&f->m);
        goto end;
    }
    SCMutexUnlock(&f->m);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);
    if (PacketAlertCheck(p, 1)) {
        printf("sig 1 alerted: ");
        goto end;
    }

    http_state = f->alstate;
    if (http_state == NULL) {
        printf("no http state: ");
        result = 0;
        goto end;
    }

    if (http_state->files_ts == NULL) {
        printf("no files in state: ");
        goto end;
    }

    SCMutexLock(&f->m);
    FileContainer *files = AppLayerParserGetFiles(p->flow->proto, p->flow->alproto,
                                                  p->flow->alstate, STREAM_TOSERVER);
    if (files == NULL) {
        printf("no stored files: ");
        SCMutexUnlock(&f->m);
        goto end;
    }
    SCMutexUnlock(&f->m);

    File *file = files->head;
    if (file == NULL) {
        printf("no file: ");
        goto end;
    }

    if (file->flags & FILE_STORE) {
        printf("file is set to store, but sig didn't match: ");
        goto end;
    }

    result = 1;
end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    UTHFreeFlow(f);

    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }
    StreamTcpFreeConfig(TRUE);
    return result;
}

#endif

void DeStateRegisterTests(void)
{
#ifdef UNITTESTS
    UtRegisterTest("DeStateTest01", DeStateTest01, 1);
    UtRegisterTest("DeStateTest02", DeStateTest02, 1);
    UtRegisterTest("DeStateTest03", DeStateTest03, 1);
    UtRegisterTest("DeStateSigTest01", DeStateSigTest01, 1);
    UtRegisterTest("DeStateSigTest02", DeStateSigTest02, 1);
    UtRegisterTest("DeStateSigTest03", DeStateSigTest03, 1);
    UtRegisterTest("DeStateSigTest04", DeStateSigTest04, 1);
    UtRegisterTest("DeStateSigTest05", DeStateSigTest05, 1);
    UtRegisterTest("DeStateSigTest06", DeStateSigTest06, 1);
    UtRegisterTest("DeStateSigTest07", DeStateSigTest07, 1);
#endif

    return;
}

/**
 * @}
 */
