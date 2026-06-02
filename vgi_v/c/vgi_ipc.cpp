// VGI Arrow IPC bridge for V workers (stdio transport).
#include "vgi_ipc.h"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/io/stdio.h>
#include <arrow/ipc/api.h>
#include <arrow/ipc/dictionary.h>
#include <arrow/ipc/message.h>
#include <arrow/ipc/writer.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

namespace {

void set_err(char *err, size_t cap, const std::string &msg) {
	if (err != nullptr && cap > 0) {
		std::snprintf(err, cap, "%s", msg.c_str());
	}
}

std::shared_ptr<arrow::io::InputStream> fd_input(int fd) {
	if (fd == 0) {
		return std::make_shared<arrow::io::StdinStream>();
	}
	auto res = arrow::io::ReadableFile::Open(fd);
	if (!res.ok()) {
		return nullptr;
	}
	return *res;
}

std::shared_ptr<arrow::io::OutputStream> fd_output(int fd) {
	if (fd == 1) {
		return std::make_shared<arrow::io::StdoutStream>();
	}
	auto out = arrow::io::FileOutputStream::Open(fd);
	if (!out.ok()) {
		return nullptr;
	}
	return *out;
}

int metadata_string(const std::shared_ptr<const arrow::KeyValueMetadata> &md, const char *key, char *out,
                    size_t out_cap) {
	if (md == nullptr) {
		return -1;
	}
	auto idx = md->FindKey(key);
	if (idx < 0) {
		return -1;
	}
	const auto &v = md->value(idx);
	std::snprintf(out, out_cap, "%s", v.c_str());
	return 0;
}

std::string json_escape(const std::string &s) {
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			out += c;
			break;
		}
	}
	return out;
}

std::shared_ptr<arrow::Schema> schema_from_wire_blob(const uint8_t *blob, size_t len) {
	if (blob == nullptr || len == 0) {
		return arrow::schema({});
	}
	auto buffer = std::make_shared<arrow::Buffer>(blob, static_cast<int64_t>(len));
	auto input = std::make_shared<arrow::io::BufferReader>(buffer);
	auto reader_res = arrow::ipc::RecordBatchStreamReader::Open(input);
	if (!reader_res.ok()) {
		return nullptr;
	}
	return (*reader_res)->schema();
}

} // namespace

extern "C" {

static int write_bytes_no_close_fd(int fd, const uint8_t *blob, size_t blob_len, char *err,
                                   size_t err_cap);

void vgi_ipc_free(void *p) {
	std::free(p);
}

static int payload_from_batch(const std::shared_ptr<arrow::RecordBatch> &batch, VgiRpcRequest *out,
                              char *err, size_t err_cap) {
	if (batch->num_columns() == 1 && batch->column_name(0) == "request") {
		auto col = std::static_pointer_cast<arrow::BinaryArray>(batch->column(0));
		if (batch->num_rows() < 1) {
			set_err(err, err_cap, "request batch has no rows");
			return -1;
		}
		auto view = col->GetView(0);
		out->payload_len = view.size();
		out->payload = static_cast<uint8_t *>(std::malloc(out->payload_len));
		if (out->payload == nullptr) {
			set_err(err, err_cap, "malloc failed");
			return -1;
		}
		std::memcpy(out->payload, view.data(), out->payload_len);
		return 0;
	}
	arrow::ipc::IpcWriteOptions opts = arrow::ipc::IpcWriteOptions::Defaults();
	auto buf_res = arrow::ipc::SerializeRecordBatch(*batch, opts);
	if (!buf_res.ok()) {
		set_err(err, err_cap, buf_res.status().ToString());
		return -1;
	}
	auto buf = *buf_res;
	out->payload_len = static_cast<size_t>(buf->size());
	out->payload = static_cast<uint8_t *>(std::malloc(out->payload_len));
	if (out->payload == nullptr) {
		set_err(err, err_cap, "malloc failed");
		return -1;
	}
	std::memcpy(out->payload, buf->data(), out->payload_len);
	return 0;
}

// After init/exchange RPC, year batches may follow on the same stdin IPC stream.
static thread_local std::shared_ptr<arrow::ipc::RecordBatchStreamReader> g_rpc_reader;

static bool retain_reader_for_method(const char *method) {
	return std::strcmp(method, "init") == 0 || std::strcmp(method, "exchange") == 0;
}

static bool drain_rpc_stream_for_method(const char *method) {
	return !retain_reader_for_method(method);
}

static int read_request_impl(int fd, char *method, size_t method_cap, uint8_t **payload_out,
                             size_t *payload_len_out, char *err, size_t err_cap) {
	if (method == nullptr || method_cap == 0 || payload_out == nullptr || payload_len_out == nullptr) {
		set_err(err, err_cap, "null out parameter");
		return -1;
	}
	method[0] = '\0';
	*payload_out = nullptr;
	*payload_len_out = 0;
	auto input = fd_input(fd);
	auto reader_res = arrow::ipc::RecordBatchStreamReader::Open(input);
	if (!reader_res.ok()) {
		set_err(err, err_cap, reader_res.status().ToString());
		return -1;
	}
	auto reader = *reader_res;

	std::shared_ptr<arrow::RecordBatch> request_batch;
	while (true) {
		auto rbm_res = reader->ReadNext();
		if (!rbm_res.ok()) {
			set_err(err, err_cap, rbm_res.status().ToString());
			return -1;
		}
		auto rbm = *rbm_res;
		if (rbm.batch == nullptr) {
			if (request_batch == nullptr) {
				set_err(err, err_cap, "empty request stream (no record batch)");
				return -1;
			}
			break;
		}
		if (request_batch == nullptr) {
			request_batch = rbm.batch;
			char method_buf[128] = {};
			if (metadata_string(rbm.custom_metadata, "vgi_rpc.method", method_buf,
			                    sizeof(method_buf)) != 0) {
				auto schema_md = request_batch->schema()->metadata();
				if (metadata_string(schema_md, "vgi_rpc.method", method_buf, sizeof(method_buf)) != 0) {
					set_err(err, err_cap, "missing vgi_rpc.method");
					return -1;
				}
			}
			std::strncpy(method, method_buf, method_cap - 1);
			method[method_cap - 1] = '\0';
			if (!drain_rpc_stream_for_method(method)) {
				g_rpc_reader = reader;
				break;
			}
		}
	}
	if (drain_rpc_stream_for_method(method)) {
		while (true) {
			auto rbm_res = reader->ReadNext();
			if (!rbm_res.ok()) {
				set_err(err, err_cap, rbm_res.status().ToString());
				return -1;
			}
			if ((*rbm_res).batch == nullptr) {
				break;
			}
		}
	}
	VgiRpcRequest tmp{};
	const int payload_rc = payload_from_batch(request_batch, &tmp, err, err_cap);
	*payload_out = tmp.payload;
	*payload_len_out = tmp.payload_len;
	return payload_rc;
}

int vgi_ipc_read_request_v(int fd, char *method, size_t method_cap, uint8_t **payload_out,
                           size_t *payload_len_out, char *err, size_t err_cap) {
	return read_request_impl(fd, method, method_cap, payload_out, payload_len_out, err, err_cap);
}

int vgi_ipc_read_request(int fd, VgiRpcRequest *out, char *err, size_t err_cap) {
	if (out == nullptr) {
		set_err(err, err_cap, "out is null");
		return -1;
	}
	return read_request_impl(fd, out->method, sizeof(out->method), &out->payload, &out->payload_len, err,
	                       err_cap);
}

int vgi_ipc_write_unary_binary(int fd, const uint8_t *blob, size_t blob_len, char *err, size_t err_cap) {
	auto output = fd_output(fd);
	if (output == nullptr) {
		set_err(err, err_cap, "fd_output failed");
		return -1;
	}
	auto schema = arrow::schema({arrow::field("result", arrow::binary())});
	auto writer_res = arrow::ipc::MakeStreamWriter(output, schema);
	if (!writer_res.ok()) {
		set_err(err, err_cap, writer_res.status().ToString());
		return -1;
	}
	auto writer = *writer_res;
	arrow::BinaryBuilder builder;
	auto ast = builder.Append(blob, static_cast<int32_t>(blob_len));
	if (!ast.ok()) {
		set_err(err, err_cap, ast.ToString());
		return -1;
	}
	std::shared_ptr<arrow::Array> arr;
	ast = builder.Finish(&arr);
	if (!ast.ok()) {
		set_err(err, err_cap, ast.ToString());
		return -1;
	}
	auto batch = arrow::RecordBatch::Make(schema, 1, {arr});
	auto st = writer->WriteRecordBatch(*batch);
	if (!st.ok()) {
		set_err(err, err_cap, st.ToString());
		return -1;
	}
	st = writer->Close();
	if (!st.ok()) {
		set_err(err, err_cap, st.ToString());
		return -1;
	}
	return 0;
}

int vgi_ipc_write_stream_blob(int fd, const uint8_t *blob, size_t blob_len, char *err, size_t err_cap) {
	return write_bytes_no_close_fd(fd, blob, blob_len, err, err_cap);
}

struct StreamReaderState {
	std::shared_ptr<arrow::ipc::RecordBatchStreamReader> reader;
	bool opened = false;
};

static thread_local std::unique_ptr<StreamReaderState> g_in_reader;

static int read_int64_batch_from_reader(const std::shared_ptr<arrow::ipc::RecordBatchStreamReader> &reader,
                                        int64_t **years_out, uint8_t **valid_out, int64_t *n_out, char *err,
                                        size_t err_cap) {
	std::shared_ptr<arrow::RecordBatch> batch;
	auto st = reader->ReadNext(&batch);
	if (!st.ok()) {
		set_err(err, err_cap, st.ToString());
		return -1;
	}
	if (batch == nullptr) {
		return 1;
	}
	if (batch->num_columns() < 1) {
		set_err(err, err_cap, "input batch has no columns");
		return -1;
	}
	auto col = std::static_pointer_cast<arrow::Int64Array>(batch->column(0));
	int64_t n = batch->num_rows();
	*years_out = static_cast<int64_t *>(std::malloc(sizeof(int64_t) * n));
	*valid_out = static_cast<uint8_t *>(std::malloc(n));
	if (*years_out == nullptr || *valid_out == nullptr) {
		set_err(err, err_cap, "malloc failed");
		return -1;
	}
	for (int64_t i = 0; i < n; i++) {
		if (col->IsNull(i)) {
			(*valid_out)[i] = 0;
			(*years_out)[i] = 0;
		} else {
			(*valid_out)[i] = 1;
			(*years_out)[i] = col->Value(i);
		}
	}
	*n_out = n;
	return 0;
}

int vgi_ipc_read_int64_column(int fd, int64_t **years_out, uint8_t **valid_out, int64_t *n_out, char *err,
                              size_t err_cap) {
	if (g_rpc_reader) {
		const int rc = read_int64_batch_from_reader(g_rpc_reader, years_out, valid_out, n_out, err, err_cap);
		if (rc == 1) {
			g_rpc_reader.reset();
		} else {
			return rc;
		}
	}
	if (!g_in_reader) {
		g_in_reader = std::make_unique<StreamReaderState>();
	}
	if (!g_in_reader->opened) {
		auto input = fd_input(fd);
		auto reader_res = arrow::ipc::RecordBatchStreamReader::Open(input);
		if (!reader_res.ok()) {
			set_err(err, err_cap, reader_res.status().ToString());
			return -1;
		}
		g_in_reader->reader = *reader_res;
		g_in_reader->opened = true;
	}
	return read_int64_batch_from_reader(g_in_reader->reader, years_out, valid_out, n_out, err, err_cap);
}

static thread_local std::shared_ptr<arrow::ipc::RecordBatchWriter> g_out_writer;

int vgi_ipc_write_date32_result(int fd, const int32_t *days, const uint8_t *valid, int64_t n, char *err,
                                size_t err_cap) {
	if (!g_out_writer) {
		auto output = fd_output(fd);
		if (output == nullptr) {
			set_err(err, err_cap, "fd_output failed");
			return -1;
		}
		auto schema = arrow::schema({arrow::field("result", arrow::date32())});
		auto writer_res = arrow::ipc::MakeStreamWriter(output, schema);
		if (!writer_res.ok()) {
			set_err(err, err_cap, writer_res.status().ToString());
			return -1;
		}
		g_out_writer = *writer_res;
	}
	arrow::Date32Builder builder;
	arrow::Status ast;
	for (int64_t i = 0; i < n; i++) {
		if (valid[i] == 0) {
			ast = builder.AppendNull();
		} else {
			ast = builder.Append(days[i]);
		}
		if (!ast.ok()) {
			set_err(err, err_cap, ast.ToString());
			return -1;
		}
	}
	std::shared_ptr<arrow::Array> arr;
	ast = builder.Finish(&arr);
	if (!ast.ok()) {
		set_err(err, err_cap, ast.ToString());
		return -1;
	}
	auto schema = arrow::schema({arrow::field("result", arrow::date32())});
	auto batch = arrow::RecordBatch::Make(schema, n, {arr});
	auto st = g_out_writer->WriteRecordBatch(*batch);
	if (!st.ok()) {
		set_err(err, err_cap, st.ToString());
		return -1;
	}
	return 0;
}

int vgi_ipc_write_int64_input_stream(int fd, const int64_t *years, int64_t n, char *err,
                                     size_t err_cap) {
	// Build IPC stream in memory so pipe fds are written with plain write() (no lseek).
	auto sink_res = arrow::io::BufferOutputStream::Create();
	if (!sink_res.ok()) {
		set_err(err, err_cap, sink_res.status().ToString());
		return -1;
	}
	auto sink = *sink_res;
	auto schema = arrow::schema({arrow::field("year", arrow::int64())});
	auto writer_res = arrow::ipc::MakeStreamWriter(sink, schema);
	if (!writer_res.ok()) {
		set_err(err, err_cap, writer_res.status().ToString());
		return -1;
	}
	auto writer = *writer_res;
	arrow::Int64Builder builder;
	for (int64_t i = 0; i < n; i++) {
		auto ast = builder.Append(years[i]);
		if (!ast.ok()) {
			set_err(err, err_cap, ast.ToString());
			return -1;
		}
	}
	std::shared_ptr<arrow::Array> arr;
	auto ast = builder.Finish(&arr);
	if (!ast.ok()) {
		set_err(err, err_cap, ast.ToString());
		return -1;
	}
	auto batch = arrow::RecordBatch::Make(schema, n, {arr});
	auto st = writer->WriteRecordBatch(*batch);
	if (!st.ok()) {
		set_err(err, err_cap, st.ToString());
		return -1;
	}
	st = writer->Close();
	if (!st.ok()) {
		set_err(err, err_cap, st.ToString());
		return -1;
	}
	auto buf_res = sink->Finish();
	if (!buf_res.ok()) {
		set_err(err, err_cap, buf_res.status().ToString());
		return -1;
	}
	auto buf = *buf_res;
	return write_bytes_no_close_fd(fd, buf->data(), static_cast<size_t>(buf->size()), err, err_cap);
}

void vgi_ipc_reset_streams(void) {
	g_rpc_reader.reset();
	g_in_reader.reset();
	g_out_writer.reset();
}

static int write_bytes_no_close_fd(int fd, const uint8_t *blob, size_t blob_len, char *err,
                                   size_t err_cap) {
	auto out = fd_output(fd);
	if (out == nullptr) {
		set_err(err, err_cap, "fd_output failed");
		return -1;
	}
	auto st = out->Write(blob, static_cast<int64_t>(blob_len));
	if (!st.ok()) {
		set_err(err, err_cap, st.ToString());
		return -1;
	}
	st = out->Flush();
	if (!st.ok()) {
		set_err(err, err_cap, st.ToString());
		return -1;
	}
	// Keep stdin/stdout open for the next RPC on the same pipe pair.
	if (fd != 0 && fd != 1) {
		st = out->Close();
		if (!st.ok()) {
			set_err(err, err_cap, st.ToString());
			return -1;
		}
	}
	return 0;
}

int vgi_ipc_write_raw(int fd, const uint8_t *blob, size_t blob_len, char *err, size_t err_cap) {
	return write_bytes_no_close_fd(fd, blob, blob_len, err, err_cap);
}

int vgi_ipc_write_rpc_error(int fd, const char *error_type, const char *error_message,
                            const char *error_kind, const uint8_t *schema_wire, size_t schema_wire_len,
                            char *err, size_t err_cap) {
	if (error_type == nullptr) {
		error_type = "Error";
	}
	if (error_message == nullptr) {
		error_message = "unknown error";
	}
	g_rpc_reader.reset();
	g_in_reader.reset();

	auto schema = schema_from_wire_blob(schema_wire, schema_wire_len);
	if (schema == nullptr) {
		set_err(err, err_cap, "schema_from_wire_blob failed");
		return -1;
	}

	std::ostringstream extra_json;
	extra_json << "{\"exception_type\":\"" << json_escape(error_type) << "\",\"exception_message\":\""
	           << json_escape(error_message) << "\",\"traceback\":\"\"";
	if (error_kind != nullptr && error_kind[0] != '\0') {
		extra_json << ",\"error_kind\":\"" << json_escape(error_kind) << "\"";
	}
	extra_json << "}";

	std::string summary = std::string(error_type) + ": " + error_message;

	std::vector<std::string> md_keys = {"vgi_rpc.log_level", "vgi_rpc.log_message", "vgi_rpc.log_extra"};
	std::vector<std::string> md_values = {"EXCEPTION", summary, extra_json.str()};
	if (error_kind != nullptr && error_kind[0] != '\0') {
		md_keys.emplace_back("vgi_rpc.error_kind");
		md_values.emplace_back(error_kind);
	}
	std::shared_ptr<const arrow::KeyValueMetadata> custom_md =
	    arrow::KeyValueMetadata::Make(std::move(md_keys), std::move(md_values));

	auto output = fd_output(fd);
	if (output == nullptr) {
		set_err(err, err_cap, "fd_output failed");
		return -1;
	}
	arrow::ipc::IpcWriteOptions opts = arrow::ipc::IpcWriteOptions::Defaults();
	arrow::ipc::DictionaryFieldMapper mapper;
	int32_t metadata_length = 0;

	arrow::ipc::IpcPayload schema_payload;
	auto st = arrow::ipc::GetSchemaPayload(*schema, opts, mapper, &schema_payload);
	if (!st.ok()) {
		set_err(err, err_cap, st.ToString());
		return -1;
	}
	st = arrow::ipc::WriteIpcPayload(schema_payload, opts, output.get(), &metadata_length);
	if (!st.ok()) {
		set_err(err, err_cap, st.ToString());
		return -1;
	}

	auto batch_res = arrow::RecordBatch::MakeEmpty(schema);
	if (!batch_res.ok()) {
		set_err(err, err_cap, batch_res.status().ToString());
		return -1;
	}
	auto batch = *batch_res;
	arrow::ipc::IpcPayload batch_payload;
	st = arrow::ipc::GetRecordBatchPayload(*batch, custom_md, opts, &batch_payload);
	if (!st.ok()) {
		set_err(err, err_cap, st.ToString());
		return -1;
	}
	st = arrow::ipc::WriteIpcPayload(batch_payload, opts, output.get(), &metadata_length);
	if (!st.ok()) {
		set_err(err, err_cap, st.ToString());
		return -1;
	}

	// Stream EOS (matches RecordBatchStreamWriter::Close).
	constexpr int32_t k_ipc_continuation = -1;
	constexpr int32_t k_zero_length = 0;
	if (!opts.write_legacy_ipc_format) {
		st = output->Write(reinterpret_cast<const uint8_t *>(&k_ipc_continuation),
		                   sizeof(k_ipc_continuation));
		if (!st.ok()) {
			set_err(err, err_cap, st.ToString());
			return -1;
		}
	}
	st = output->Write(reinterpret_cast<const uint8_t *>(&k_zero_length), sizeof(k_zero_length));
	if (!st.ok()) {
		set_err(err, err_cap, st.ToString());
		return -1;
	}
	st = output->Flush();
	if (!st.ok()) {
		set_err(err, err_cap, st.ToString());
		return -1;
	}
	return 0;
}

int vgi_ipc_close_input_stream(int fd, char *err, size_t err_cap) {
	(void)fd;
	if (g_out_writer) {
		auto st = g_out_writer->Close();
		g_out_writer.reset();
		if (!st.ok()) {
			set_err(err, err_cap, st.ToString());
			return -1;
		}
	}
	g_rpc_reader.reset();
	g_in_reader.reset();
	return 0;
}

} // extern "C"
