// Minimal VGI stdio client for integration tests.
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

ssize_t write_all(int fd, const uint8_t *data, size_t len) {
	size_t off = 0;
	while (off < len) {
		auto n = write(fd, data + off, len - off);
		if (n <= 0) {
			return -1;
		}
		off += static_cast<size_t>(n);
	}
	return static_cast<ssize_t>(off);
}

std::vector<uint8_t> read_exact(int fd, size_t nbytes) {
	std::vector<uint8_t> buf(nbytes);
	size_t off = 0;
	while (off < nbytes) {
		const ssize_t n = read(fd, buf.data() + off, nbytes - off);
		if (n <= 0) {
			throw std::runtime_error("read_exact: short read");
		}
		off += static_cast<size_t>(n);
	}
	return buf;
}

std::vector<uint8_t> read_until_eof(int fd) {
	std::vector<uint8_t> buf;
	uint8_t chunk[8192];
	while (true) {
		const ssize_t n = read(fd, chunk, sizeof(chunk));
		if (n == 0) {
			break;
		}
		if (n < 0) {
			throw std::runtime_error("read_until_eof failed");
		}
		buf.insert(buf.end(), chunk, chunk + n);
	}
	return buf;
}

std::vector<int32_t> parse_date32_stream(const std::vector<uint8_t> &ipc_bytes) {
	std::vector<int32_t> days;
	auto buffer = std::make_shared<arrow::Buffer>(reinterpret_cast<const uint8_t *>(ipc_bytes.data()),
	                                              static_cast<int64_t>(ipc_bytes.size()));
	auto input = std::make_shared<arrow::io::BufferReader>(buffer);
	auto reader_res = arrow::ipc::RecordBatchStreamReader::Open(input);
	if (!reader_res.ok()) {
		throw std::runtime_error(reader_res.status().ToString());
	}
	auto reader = *reader_res;
	while (true) {
		auto batch_res = reader->Next();
		if (!batch_res.ok()) {
			throw std::runtime_error(batch_res.status().ToString());
		}
		auto batch = *batch_res;
		if (batch == nullptr) {
			break;
		}
		auto col = std::static_pointer_cast<arrow::Date32Array>(batch->column(0));
		for (int64_t i = 0; i < batch->num_rows(); i++) {
			if (col->IsNull(i)) {
				throw std::runtime_error("unexpected null date32");
			}
			days.push_back(col->Value(i));
		}
	}
	return days;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 3) {
		std::cerr << "usage: vgi_smoke_client <easter_worker> <testdata_dir>\n";
		return 2;
	}
	const std::string worker = argv[1];
	const std::string tdir = argv[2];

	try {
		const auto bind_req = read_file(tdir + "/bind_response_params_wire.bin");
		const auto init_req = read_file(tdir + "/init_header_params_wire.bin");
		const auto bind_golden = read_file(tdir + "/bind_response_wire.bin");
		const auto init_golden = read_file(tdir + "/init_header_wire.bin");

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

		if (write_all(to_child[1], bind_req.data(), bind_req.size()) < 0) {
			throw std::runtime_error("write bind request failed");
		}
		auto bind_resp = read_exact(from_child[0], bind_golden.size());
		if (bind_resp != bind_golden) {
			std::cerr << "bind response mismatch: got " << bind_resp.size() << " want "
			          << bind_golden.size() << "\n";
			return 1;
		}

		if (write_all(to_child[1], init_req.data(), init_req.size()) < 0) {
			throw std::runtime_error("write init request failed");
		}
		auto init_resp = read_exact(from_child[0], init_golden.size());
		if (init_resp != init_golden) {
			std::cerr << "init header mismatch: got " << init_resp.size() << " want "
			          << init_golden.size() << "\n";
			return 1;
		}

		const int64_t years[] = {2024, 2025, 2026};
		char err_buf[VGI_IPC_ERR_CAP] = {};
		if (vgi_ipc_write_int64_input_stream(to_child[1], years, 3, err_buf, sizeof(err_buf)) != 0) {
			throw std::runtime_error(err_buf[0] ? err_buf : "write year stream failed");
		}
		close(to_child[1]);

		// Must match vgi_v/computus_test.v test_date32_smoke_expected (date32_days).
		const int32_t expected[] = {19813, 20198, 20548};
		// Remaining stdout bytes are the date32 result stream.
		auto out_tail = read_until_eof(from_child[0]);
		close(from_child[0]);
		int status = 0;
		waitpid(pid, &status, 0);
		auto got = parse_date32_stream(out_tail);

		if (got.size() != 3) {
			std::cerr << "expected 3 date32 values, got " << got.size() << "\n";
			return 1;
		}
		for (int i = 0; i < 3; i++) {
			if (got[i] != expected[i]) {
				std::cerr << "date32[" << i << "]=" << got[i] << " want " << expected[i] << "\n";
				return 1;
			}
		}
		std::cout << "OK smoke_client: easter_date [2024-03-31, 2025-04-20, 2026-04-05]\n";
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << "smoke_client error: " << ex.what() << "\n";
		return 1;
	}
}
