/* -*- C -*- */
/*
 * Copyright (c) 2012-2020 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */


#include "be/dtm0_log.h"
#include "dtm0/tx_desc.h"
#include "dtm0/tx_desc_xc.h"
#include "lib/errno.h"
#include "lib/assert.h"
#include "lib/memory.h"
#include "lib/chan.h"
#include "lib/finject.h"
#include "lib/time.h"
#include "lib/trace.h"
#include "lib/misc.h"           /* M0_IN() */
#include "fop/fop.h"
#include "fop/fom.h"
#include "fop/fom_generic.h"

/* #include "lib/user_space/misc.h" */
#include "rpc/rpc.h"
#include "rpc/rpclib.h"
#include "fop/fop_item_type.h"

#include "dtm0/fop.h"
#include "dtm0/fop_xc.h"
#include "dtm0/service.h"
#include "rpc/rpc_opcodes.h"

static void dtm0_rpc_item_reply_cb(struct m0_rpc_item *item);

/*
  RPC item operations structures.
 */
const struct m0_rpc_item_ops dtm0_req_fop_rpc_item_ops = {
        .rio_replied = dtm0_rpc_item_reply_cb,
};

struct m0_fop_type dtm0_req_fop_fopt;
struct m0_fop_type dtm0_rep_fop_fopt;

/*
  Fom specific routines for corresponding fops.
 */
static int dtm0_fom_tick(struct m0_fom *fom);
static int dtm0_fom_create(struct m0_fop *fop, struct m0_fom **out,
			       struct m0_reqh *reqh);
static void dtm0_fom_fini(struct m0_fom *fom);
static size_t dtm0_fom_locality(const struct m0_fom *fom);

static const struct m0_fom_ops dtm0_req_fom_ops = {
	.fo_fini = dtm0_fom_fini,
	.fo_tick = dtm0_fom_tick,
	.fo_home_locality = dtm0_fom_locality
};

extern struct m0_reqh_service_type dtm0_service_type;

enum dtm0_phases {
	DTM0_REQ_WHATEVER = M0_FOPH_NR + 1,
};

static const struct m0_fom_type_ops dtm0_req_fom_type_ops = {
        .fto_create = dtm0_fom_create,
};

static void dtm0_rpc_item_reply_cb(struct m0_rpc_item *item)
{
	struct m0_fop *reply = NULL;

	M0_ENTRY("item=%p", item);

        M0_PRE(item != NULL);
	M0_PRE(M0_IN(m0_fop_opcode(m0_rpc_item_to_fop(item)),
		     (M0_DTM0_REQ_OPCODE)));

	if (m0_rpc_item_error(item) == 0) {
		reply = m0_rpc_item_to_fop(item->ri_reply);
		M0_ASSERT(M0_IN(m0_fop_opcode(reply), (M0_DTM0_REP_OPCODE)));
	}

	M0_LEAVE("reply=%p", reply);
}

M0_INTERNAL void m0_dtm0_fop_fini(void)
{
	m0_fop_type_fini(&dtm0_req_fop_fopt);
	m0_fop_type_fini(&dtm0_rep_fop_fopt);
	m0_xc_dtm0_fop_fini();
}

M0_INTERNAL int m0_dtm0_fop_init(void)
{
	static int init_once = 0;
	if (init_once++ > 0)
		return 0;

	m0_xc_dtm0_fop_init();
	M0_FOP_TYPE_INIT(&dtm0_req_fop_fopt,
			 .name      = "DTM0 request",
			 .opcode    = M0_DTM0_REQ_OPCODE,
			 .xt        = dtm0_req_fop_xc,
			 .rpc_flags = M0_RPC_MUTABO_REQ,
			 .fom_ops   = &dtm0_req_fom_type_ops,
			 .sm        = &m0_generic_conf,
			 .svc_type  = &dtm0_service_type);
	M0_FOP_TYPE_INIT(&dtm0_rep_fop_fopt,
			 .name      = "DTM0 reply",
			 .opcode    = M0_DTM0_REP_OPCODE,
			 .xt        = dtm0_rep_fop_xc,
			 .rpc_flags = M0_RPC_ITEM_TYPE_REPLY,
			 .fom_ops   = &dtm0_req_fom_type_ops);
	return 0;
}

/*
  Allocates a fop.
 */

int m0_dtm0_fop_create(struct m0_rpc_session	 *session,
		       enum m0_dtm0s_msg	  opmsg,
		       struct m0_dtm0_tx_desc	 *txr,
		       struct m0_fop		**out)
{
	struct dtm0_req_fop  *op;
	struct m0_fop	     *fop;
	int                rc = 0;

	*out = NULL;

	fop = m0_fop_alloc_at(session, &dtm0_req_fop_fopt);
	if (fop == NULL)
		return M0_ERR(-ENOMEM);
	op = m0_fop_data(fop);
	op->dtr_msg = opmsg;
	op->dtr_txr = *txr;
	*out = fop;
	return rc;
}
M0_EXPORTED(m0_dtm0_fop_create);

/*
  Allocates a fom.
 */
static int dtm0_fom_create(struct m0_fop *fop,
			  struct m0_fom **out, struct m0_reqh *reqh)
{
	struct dtm0_fom    *fom;
	struct m0_fom     *fom0;
	struct m0_fop     *repfop;
	struct dtm0_rep_fop *reply;

	M0_ALLOC_PTR(fom);
	M0_ENTRY("reqh=%p", reqh);

	repfop = m0_fop_reply_alloc(fop, &dtm0_rep_fop_fopt);
	if (fom != NULL && repfop != NULL) {
		*out = fom0 = &fom->dtf_fom;
		/** TODO: calculate credits for the operation.
		 */

		reply = m0_fop_data(repfop);
		reply->dr_txr = (struct m0_dtm0_tx_desc) {};
		reply->dr_rc = 0;
		m0_fom_init(fom0, &fop->f_type->ft_fom_type,
			    &dtm0_req_fom_ops, fop, repfop, reqh);
		M0_LEAVE();
		return M0_RC(0);
	} else {
		m0_free(repfop);
		m0_free(fom);
		return M0_ERR(-ENOMEM);
	}
}

static void dtm0_fom_fini(struct m0_fom *fom)
{
	M0_PRE(fom != NULL);

        m0_fom_fini(fom);
        m0_free(fom);
}

static size_t dtm0_fom_locality(const struct m0_fom *fom)
{
	static int locality = 0;
	M0_PRE(fom != NULL);
	return locality++;
}

static void m0_dtm0_post__persistent(struct m0_dtm0_service *dtms,
				     const struct m0_fid *tgt,
				     const struct m0_dtm0_tx_desc *txd)
{
	struct m0_fop          *fop;
	struct m0_rpc_session  *session;
	struct m0_rpc_item     *item;
	struct dtm0_req_fop    *req;
	int                     rc;

	M0_ENTRY("reqh=%p", dtms->dos_generic.rs_reqh);

	session = m0_dtm0_service_process_session_get(&dtms->dos_generic, tgt);

	fop               = m0_fop_alloc_at(session, &dtm0_req_fop_fopt);
	item              = &fop->f_item;
	item->ri_ops      = &dtm0_req_fop_rpc_item_ops;
	item->ri_session  = session;
	item->ri_prio     = M0_RPC_ITEM_PRIO_MID;
	item->ri_deadline = M0_TIME_IMMEDIATELY;

	req               = m0_fop_data(fop);
	req->dtr_msg      = DTM_PERSISTENT;
	rc = m0_dtm0_tx_desc_copy(txd, &req->dtr_txr);
	M0_ASSERT(rc == 0);

	rc = m0_rpc_post(item);
	M0_ASSERT(rc == 0);
	m0_fop_put_lock(fop);

	M0_LEAVE("Sent PERSISTENT notice " FID_F " -> " FID_F,
		 FID_P(&dtms->dos_generic.rs_service_fid), FID_P(tgt));
}


M0_INTERNAL void m0_dtm0_on_persistent(struct m0_reqh               *reqh,
				       const struct m0_dtm0_tx_desc *txd)
{
	int                     rc;
	struct m0_dtm0_service *dtms;
	struct m0_dtm0_tx_desc  msg = {};
	int                     i;

	dtms = m0_dtm0_service_find(reqh);

	rc = m0_dtm0_tx_desc_copy(txd, &msg);
	M0_ASSERT(rc == 0);

	/* TODO: This change will be done by DTM0 log because
	 * it should log the entry "pa == self" with PERSISTENT state
	 * set.
	 */
	for (i = 0; i < msg.dtd_pg.dtpg_nr; ++i) {
		if (m0_fid_eq(&msg.dtd_pg.dtpg_pa[i].pa_fid,
			      &dtms->dos_generic.rs_service_fid)) {
			msg.dtd_pg.dtpg_pa[i].pa_state =
				max_check(msg.dtd_pg.dtpg_pa[i].pa_state,
					  (uint32_t) M0_DTPS_PERSISTENT);
		}
	}

	/* Notify the originator */
	m0_dtm0_post__persistent(dtms, &msg.dtd_id.dti_fid, &msg);

	/* TODO: Send notices to the rest of the participants. */
	m0_dtm0_tx_desc_fini(&msg);
}



#define M0_FID(c_, k_)  { .f_container = c_, .f_key = k_ }
static void dtm0_pong_back(enum m0_be_dtm0_log_credit_op msgtype,
			   struct m0_dtm0_tx_desc *txr,
			   struct m0_rpc_session  *session)
{
        struct m0_fop         *fop;
	struct dtm0_req_fop   *req;
	struct m0_rpc_item    *item;
	int                    rc;

	M0_PRE(session != NULL);

	fop               = m0_fop_alloc_at(session, &dtm0_req_fop_fopt);
	req               = m0_fop_data(fop);
	req->dtr_msg      = msgtype;
	req->dtr_txr      = *txr;
	item              = &fop->f_item;
	item->ri_ops      = &dtm0_req_fop_rpc_item_ops;
	item->ri_session  = session;
	item->ri_prio     = M0_RPC_ITEM_PRIO_MID;
	item->ri_deadline = M0_TIME_IMMEDIATELY;

	rc = m0_rpc_post(item);
	M0_ASSERT(rc == 0);
	/* XXX: put it back, we do not need it anymore.
	 * Should it be done by dtm0_rpc_item_reply_cb?
	 */
	m0_fop_put_lock(fop);
}

static int dtm0_fom_tick(struct m0_fom *fom)
{
	int                     rc;
	struct dtm0_req_fop    *req;
	struct dtm0_rep_fop    *rep;
	struct m0_dtm0_service *svc;

	if (m0_fom_phase(fom) < M0_FOPH_NR) {
		rc = m0_fom_tick_generic(fom);
	} else {
		req = m0_fop_data(fom->fo_fop);
		M0_LOG(M0_DEBUG, "Processing non-generic phase %d %d, " FID_F
		       ", %p",
		       (int) req->dtr_msg,
		       (int) m0_dtm0_is_a_volatile_dtm(fom->fo_service),
		       FID_P(&fom->fo_service->rs_service_fid),
		       fom->fo_service->rs_reqh);

		if (req->dtr_msg == DMT_EXECUTE &&
		    m0_dtm0_is_a_persistent_dtm(fom->fo_service)) {
			struct m0_fid fid = M0_FID(0x7300000000000001, 0x1a);
			dtm0_pong_back(M0_DTML_PERSISTENT, &req->dtr_txr,
				m0_dtm0_service_process_session_get(
					fom->fo_service, &fid));
		}

		if (req->dtr_msg == DTM_PERSISTENT &&
		    m0_dtm0_is_a_volatile_dtm(fom->fo_service)) {
			svc = m0_dtm0_service_find(fom->fo_service->rs_reqh);
			M0_ASSERT(svc != NULL);
			m0_be_dtm0_log_post_persistent(svc->dos_log,
						       &req->dtr_txr);
		}

		rep = m0_fop_data(fom->fo_rep_fop);

		M0_SET0(&rep->dr_txr);
		rep->dr_rc = 0;
		m0_fom_phase_set(fom, M0_FOPH_SUCCESS);
		rc = M0_FSO_AGAIN;
	}

	return M0_RC(rc);
}

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
