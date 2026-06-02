// Verifies vgi_ipc_write_rpc_error produces EXCEPTION batches Haybarn/vgi-rpc expect.
#include "vgi_ipc.h"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> read_file(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw std::runtime_error("cannot open " + path);
	}
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool md_value(const std::shared_ptr<const arrow::KeyValueMetadata> &md, const char *key, std::string *out) {
	if (md == nullptr) {
		return false;
	}
	auto idx = md->FindKey(key);
	if (idx < 0) {
		return false;
	}
	*out = md->value(idx);
	return true;
}

bool is_exception_batch(const std::shared_ptr<arrow::RecordBatch> &batch,
                        const std::shared_ptr<const arrow::KeyValueMetadata> &md,
                        const std::string &needle) {
	if (batch == nullptr || batch->num_rows() != 0 || md == nullptr) {
		return false;
	}
	std::string level;
	std::string message;
	if (!md_value(md, "vgi_rpc.log_level", &level) || !md_value(md, "vgi_rpc.log_message", &message)) {
		return false;
	}
	return level == "EXCEPTION" && message.find(needle) != std::string::npos;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 4) {
		std::cerr << "usage: vgi_error_smoke_client <easter_worker> <testdata_dir> <schema_wire.bin>\n";
		return 2;
	}
	const std::string worker = argv[1];
	const std::string tdir = argv[2];
	const auto schema_wire = read_file(argv[3]);

	try {
		int pipefd[2];
		if (pipe(pipefd) != 0) {
			perror("pipe");
			return 1;
		}
		const char *msg = "catalog_attach: unknown catalog";
		char err_buf[VGI_IPC_ERR_CAP] = {};
		if (vgi_ipc_write_rpc_error(pipefd[1], "Error", msg, nullptr, schema_wire.data(),
		                            schema_wire.size(), err_buf, sizeof(err_buf)) != 0) {
			throw std::runtime_error(err_buf[0] ? err_buf : "vgi_ipc_write_rpc_error failed");
		}
		close(pipefd[1]);

		auto buffer = std::make_shared<arrow::Buffer>(reinterpret_cast<const uint8_t *>(schema_wire.data()),
		                                              static_cast<int64_t>(schema_wire.size()));
		auto schema_reader = arrow::ipc::RecordBatchStreamReader::Open(
		    std::make_shared<arrow::io::BufferReader>(buffer));
		if (!schema_reader.ok()) {
			throw std::runtime_error(schema_reader.status().ToString());
		}
		const auto expected_schema = (*schema_reader)->schema();

		auto input_res = arrow::io::ReadableFile::Open(pipefd[0]);
		if (!input_res.ok()) {
			throw std::runtime_error(input_res.status().ToString());
		}
		auto input = *input_res;
		auto reader_res = arrow::ipc::RecordBatchStreamReader::Open(input.get());
		if (!reader_res.ok()) {
			throw std::runtime_error(reader_res.status().ToString());
		}
		auto reader = *reader_res;
		if (!reader->schema()->Equals(*expected_schema)) {
			std::cerr << "error stream schema mismatch\n";
			return 1;
		}

		bool saw_exception = false;
		while (true) {
			auto rbm_res = reader->ReadNext();
			if (!rbm_res.ok()) {
				throw std::runtime_error(rbm_res.status().ToString());
			}
			auto rbm = *rbm_res;
			if (rbm.batch == nullptr) {
				break;
			}
			if (is_exception_batch(rbm.batch, rbm.custom_metadata, "unknown catalog")) {
				saw_exception = true;
			}
		}
		close(pipefd[0]);

		if (!saw_exception) {
			std::cerr << "no EXCEPTION batch in error stream\n";
			return 1;
		}

		// Worker integration: attach request whose payload omits the easter catalog name.
		const auto attach_req = read_file(tdir + "/catalog_catalog_attach_params_wire.bin");
		std::vector<uint8_t> bad_req = attach_req;
		const char needle[] = "easter";
		const char repl[] = "xxxxxx";
		bool patched = false;
		for (size_t i = 0; i + sizeof(needle) - 1 <= bad_req.size(); i++) {
			if (std::memcmp(bad_req.data() + i, needle, sizeof(needle) - 1) == 0) {
				std::memcpy(bad_req.data() + i, repl, sizeof(needle) - 1);
				patched = true;
				break;
			}
		}
		if (!patched) {
			std::cerr << "skip worker attach-error check (easter not found in attach params wire)\n";
			std::cout << "OK error_smoke: rpc EXCEPTION batch (shim only)\n";
			return 0;
		}
		int to_child[2];
		int from_child[2];
		if (pipe(to_child) != 0 || pipe(from_child) != 0) {
			perror("pipe");
			return 1;
		}
		const pid_t pid = fork();
		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid == 0) {
			close(to_child[1]);
			close(from_child[0]);
			dup2(to_child[0], STDIN_FILENO);
			dup2(from_child[1], STDOUT_FILENO);
			close(to_child[0]);
			close(from_child[1]);
			execl(worker.c_str(), worker.c_str(), static_cast<char *>(nullptr));
			perror("execl");
			_exit(127);
		}
		close(to_child[0]);
		close(from_child[1]);
		size_t off = 0;
		while (off < bad_req.size()) {
			const ssize_t n = write(to_child[1], bad_req.data() + off, bad_req.size() - off);
			if (n <= 0) {
				throw std::runtime_error("write attach request failed");
			}
			off += static_cast<size_t>(n);
		}
		close(to_child[1]);

		std::vector<uint8_t> out;
		uint8_t chunk[8192];
		while (true) {
			const ssize_t n = read(from_child[0], chunk, sizeof(chunk));
			if (n == 0) {
				break;
			}
			if (n < 0) {
				throw std::runtime_error("read worker stdout failed");
			}
			out.insert(out.end(), chunk, chunk + n);
		}
		close(from_child[0]);
		int status = 0;
		waitpid(pid, &status, 0);

		auto out_buf = std::make_shared<arrow::Buffer>(out.data(), static_cast<int64_t>(out.size()));
		auto out_reader_res =
		    arrow::ipc::RecordBatchStreamReader::Open(std::make_shared<arrow::io::BufferReader>(out_buf));
		if (!out_reader_res.ok()) {
			throw std::runtime_error(out_reader_res.status().ToString());
		}
		auto out_reader = *out_reader_res;
		bool worker_exception = false;
		while (true) {
			auto rbm_res = out_reader->ReadNext();
			if (!rbm_res.ok()) {
				throw std::runtime_error(rbm_res.status().ToString());
			}
			auto rbm = *rbm_res;
			if (rbm.batch == nullptr) {
				break;
			}
			if (is_exception_batch(rbm.batch, rbm.custom_metadata, "unknown catalog")) {
				worker_exception = true;
			}
		}
		if (!worker_exception) {
			std::cerr << "worker did not return EXCEPTION batch for bad attach\n";
			return 1;
		}

		std::cout << "OK error_smoke: rpc EXCEPTION batches (shim + worker)\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << "error_smoke_client error: " << ex.what() << "\n";
		return 1;
	}
}
