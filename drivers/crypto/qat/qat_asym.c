/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019 Intel Corporation
 */

#include <stdarg.h>

#include "qat_asym.h"
#include "icp_qat_fw_pke.h"
#include "icp_qat_fw.h"
#include "qat_pke_functionality_arrays.h"

#define qat_asym_sz_2param(arg) (arg, sizeof(arg)/sizeof(*arg))

static int __rte_unused qat_asym_get_sz_and_func_id(const uint32_t arr[][2],
		size_t arr_sz, size_t *size, uint32_t *func_id)
{
	size_t i;

	for (i = 0; i < arr_sz; i++) {
		if (*size <= arr[i][0]) {
			*size = arr[i][0];
			*func_id = arr[i][1];
			return 0;
		}
	}
	return -1;
}

static void qat_asym_build_req_tmpl(void *sess_private_data,
		struct rte_crypto_asym_xform __rte_unused *xform)
{

	struct icp_qat_fw_pke_request *qat_req;
	struct qat_asym_session *session = sess_private_data;

	qat_req = &session->req_tmpl;
	memset(qat_req, 0, sizeof(*qat_req));
	qat_req->pke_hdr.service_type = ICP_QAT_FW_COMN_REQ_CPM_FW_PKE;

	qat_req->pke_hdr.hdr_flags =
			ICP_QAT_FW_COMN_HDR_FLAGS_BUILD
			(ICP_QAT_FW_COMN_REQ_FLAG_SET);
	qat_req->pke_hdr.comn_req_flags = 0;
	qat_req->pke_hdr.resrvd1 = 0;
	qat_req->pke_hdr.resrvd2 = 0;
	qat_req->pke_hdr.kpt_mask = 0;
	qat_req->pke_hdr.kpt_rn_mask = 0;
	qat_req->pke_hdr.cd_pars.content_desc_addr = 0;
	qat_req->pke_hdr.cd_pars.content_desc_resrvd = 0;
	qat_req->resrvd1 = 0;
	qat_req->resrvd2 = 0;
	qat_req->next_req_adr = 0;
}

static size_t __rte_unused max_of(int n, ...)
{
	va_list args;
	size_t len = 0, num;
	int i;

	va_start(args, n);
	len = va_arg(args, size_t);

	for (i = 0; i < n - 1; i++) {
		num = va_arg(args, size_t);
		if (num > len)
			len = num;
	}
	va_end(args);

	return len;
}

static void __rte_unused qat_clear_arrays(struct qat_asym_op_cookie *cookie,
		int in_count, int out_count, int in_size, int out_size)
{
	int i;

	for (i = 0; i < in_count; i++)
		memset(cookie->input_array[i], 0x0, in_size);
	for (i = 0; i < out_count; i++)
		memset(cookie->output_array[i], 0x0, out_size);
}

static int __rte_unused qat_asym_check_nonzero(rte_crypto_param n)
{
	if (n.length < 8) {
		/* Not a case for any cryptograpic function except for DH
		 * generator which very often can be of one byte length
		 */
		size_t i;

		if (n.data[n.length - 1] == 0x0) {
			for (i = 0; i < n.length - 1; i++)
				if (n.data[i] != 0x0)
					break;
			if (i == n.length - 1)
				return QAT_ASYM_ERROR_INVALID_PARAM;
		}
	} else if (*(uint64_t *)&n.data[
				n.length - 8] == 0) {
		/* Very likely it is zeroed modulus */
		size_t i;

		for (i = 0; i < n.length - 8; i++)
			if (n.data[i] != 0x0)
				break;
		if (i == n.length - 8)
			return QAT_ASYM_ERROR_INVALID_PARAM;
	}

	return 0;
}

int
qat_asym_build_request(void *in_op,
			uint8_t *out_msg,
			void *op_cookie,
			__rte_unused enum qat_device_gen qat_dev_gen)
{
	struct qat_asym_session *ctx;
	struct rte_crypto_op *op = (struct rte_crypto_op *)in_op;
	struct rte_crypto_asym_op __rte_unused *asym_op = op->asym;
	struct icp_qat_fw_pke_request *qat_req =
			(struct icp_qat_fw_pke_request *)out_msg;
	struct qat_asym_op_cookie *cookie =
				(struct qat_asym_op_cookie *)op_cookie;

	ctx = (struct qat_asym_session *)get_asym_session_private_data(
			op->asym->session, cryptodev_qat_asym_driver_id);
	rte_mov64((uint8_t *)qat_req, (const uint8_t *)&(ctx->req_tmpl));
	qat_req->pke_mid.opaque = (uint64_t)(uintptr_t)op;

	qat_req->pke_mid.src_data_addr = cookie->input_addr;
	qat_req->pke_mid.dest_data_addr = cookie->output_addr;

	goto error;

#if RTE_LOG_DP_LEVEL >= RTE_LOG_DEBUG
	QAT_DP_HEXDUMP_LOG(DEBUG, "qat_req:", qat_req,
			sizeof(struct icp_qat_fw_pke_request));
#endif
	return 0;
error:
	qat_req->output_param_count = 0;
	qat_req->input_param_count = 0;
	qat_req->pke_hdr.service_type = ICP_QAT_FW_COMN_REQ_NULL;
	cookie->error |= QAT_ASYM_ERROR_INVALID_PARAM;

	return 0;
}

void
qat_asym_process_response(void **op, uint8_t *resp,
		void *op_cookie)
{
	struct icp_qat_fw_pke_resp *resp_msg =
			(struct icp_qat_fw_pke_resp *)resp;
	struct rte_crypto_op *rx_op = (struct rte_crypto_op *)(uintptr_t)
			(resp_msg->opaque);
	struct qat_asym_op_cookie *cookie = op_cookie;

	*op = rx_op;
	rx_op->status = RTE_CRYPTO_OP_STATUS_NOT_PROCESSED;

	if (cookie->error) {
		cookie->error = 0;
		rx_op->status = RTE_CRYPTO_OP_STATUS_ERROR;
		QAT_DP_LOG(ERR, "Cookie status returned error");
	} else {
		if (ICP_QAT_FW_PKE_RESP_PKE_STAT_GET(
			resp_msg->pke_resp_hdr.resp_status.pke_resp_flags)) {
			rx_op->status = RTE_CRYPTO_OP_STATUS_ERROR;
			QAT_DP_LOG(ERR, "Asymmetric response status returned error");
		}
		if (resp_msg->pke_resp_hdr.resp_status.comn_err_code) {
			rx_op->status = RTE_CRYPTO_OP_STATUS_ERROR;
			QAT_DP_LOG(ERR, "Asymmetric common status returned error");
		}
	}

#if RTE_LOG_DP_LEVEL >= RTE_LOG_DEBUG
	QAT_DP_HEXDUMP_LOG(DEBUG, "resp_msg:", resp_msg,
			sizeof(struct icp_qat_fw_pke_resp));
#endif
}

int
qat_asym_session_configure(struct rte_cryptodev *dev,
		struct rte_crypto_asym_xform *xform,
		struct rte_cryptodev_asym_session *sess,
		struct rte_mempool *mempool)
{
	int err;
	void *sess_private_data;

	err = -EINVAL;
	goto error;
	if (rte_mempool_get(mempool, &sess_private_data)) {
		QAT_LOG(ERR,
			"Couldn't get object from session mempool");
		return -ENOMEM;
	}

	qat_asym_build_req_tmpl(sess_private_data, xform);

	set_asym_session_private_data(sess, dev->driver_id,
		sess_private_data);

	return 0;
error:
	return err;
}

unsigned int qat_asym_session_get_private_size(
		struct rte_cryptodev *dev __rte_unused)
{
	return RTE_ALIGN_CEIL(sizeof(struct qat_asym_session), 8);
}

void
qat_asym_session_clear(struct rte_cryptodev *dev,
		struct rte_cryptodev_asym_session *sess)
{
	uint8_t index = dev->driver_id;
	void *sess_priv = get_asym_session_private_data(sess, index);
	struct qat_asym_session *s = (struct qat_asym_session *)sess_priv;

	if (sess_priv) {
		memset(s, 0, qat_asym_session_get_private_size(dev));
		struct rte_mempool *sess_mp = rte_mempool_from_obj(sess_priv);

		set_asym_session_private_data(sess, index, NULL);
		rte_mempool_put(sess_mp, sess_priv);
	}
}