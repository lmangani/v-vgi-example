// Apache Arrow IPC helpers for VGI stdio workers.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VGI_IPC_ERR_CAP 512

typedef struct {
	char method[128];
	uint8_t *payload;
	size_t payload_len;
} VgiRpcRequest;

// Read one RPC request IPC stream from fd (typically stdin).
// Returns 0 on success; payload is malloc'd (free with vgi_ipc_free).
int vgi_ipc_read_request(int fd, VgiRpcRequest *out, char *err, size_t err_cap);

// V-friendly out-params (avoids @[c] struct layout mismatch with VgiRpcRequest).
int vgi_ipc_read_request_v(int fd, char *method, size_t method_cap, uint8_t **payload_out,
                           size_t *payload_len_out, char *err, size_t err_cap);

// Write unary response: schema [result: binary], one row, EOS.
int vgi_ipc_write_unary_binary(int fd, const uint8_t *blob, size_t blob_len, char *err, size_t err_cap);

// Write stream header: serialize GlobalInitResponse (or any) as single-row struct batch stream.
int vgi_ipc_write_stream_blob(int fd, const uint8_t *blob, size_t blob_len, char *err, size_t err_cap);

// Read next input batch from stream; extracts int64 column 0 as years.
// valid_out[i]=1 valid, 0 null. Returns 0 on success, 1 on EOS, -1 on error.
int vgi_ipc_read_int64_column(int fd, int64_t **years_out, uint8_t **valid_out, int64_t *n_out, char *err,
                              size_t err_cap);

// Write output batch: schema [result: date32], days since Unix epoch (nullable).
int vgi_ipc_write_date32_result(int fd, const int32_t *days, const uint8_t *valid, int64_t n, char *err,
                                size_t err_cap);

// Close an open input IPC stream writer side (EOS on input stream).
int vgi_ipc_close_input_stream(int fd, char *err, size_t err_cap);

void vgi_ipc_free(void *p);

// Write raw bytes to fd (prebuilt IPC stream).
int vgi_ipc_write_raw(int fd, const uint8_t *blob, size_t blob_len, char *err, size_t err_cap);

// Write one int64-column input stream (schema [year: int64]) for init exchange tests.
int vgi_ipc_write_int64_input_stream(int fd, const int64_t *years, int64_t n, char *err,
                                     size_t err_cap);

// Reset stream readers/writers between RPC calls.
void vgi_ipc_reset_streams(void);

// Write a vgi-rpc EXCEPTION batch (zero-row stream). schema_wire may be a golden
// success-response IPC blob to reuse its result schema; pass NULL/0 for empty schema.
// error_kind may be NULL (e.g. "method_not_implemented" for unknown methods).
int vgi_ipc_write_rpc_error(int fd, const char *error_type, const char *error_message,
                            const char *error_kind, const uint8_t *schema_wire, size_t schema_wire_len,
                            char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif
