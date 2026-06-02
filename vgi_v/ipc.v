module vgi_v

$if !test {
#flag -I@VMODROOT/c
#flag -L@VMODROOT/c
$if linux {
	#flag -l:libvgi_ipc.so
} $else {
	#flag -l:libvgi_ipc.dylib
}
#flag -Wl,-rpath,@VMODROOT/c

#include "vgi_ipc.h"

pub const ipc_err_cap = 512

fn C.vgi_ipc_reset_streams()
fn C.vgi_ipc_read_request_v(fd int, method &u8, method_cap usize, payload_out &&u8, payload_len_out &usize, err &u8, err_cap usize) int

pub fn ipc_reset_streams() {
	C.vgi_ipc_reset_streams()
}
fn C.vgi_ipc_free(p voidptr)
fn C.vgi_ipc_write_raw(fd int, data &u8, len usize, err &u8, err_cap usize) int
fn C.vgi_ipc_write_rpc_error(fd int, error_type &u8, error_message &u8, error_kind &u8, schema_wire &u8,
	schema_wire_len usize, err &u8, err_cap usize) int
fn C.vgi_ipc_read_int64_column(fd int, years_out &&i64, valid_out &&u8, n_out &i64, err &u8, err_cap usize) int
fn C.vgi_ipc_write_date32_result(fd int, days &i32, valid &u8, n int, err &u8, err_cap usize) int
fn C.vgi_ipc_close_input_stream(fd int, err &u8, err_cap usize) int

pub struct RpcRequest {
	method string
	payload []u8
}

fn method_from_buf(buf &[128]u8) string {
	mut out := []u8{}
	unsafe {
		for i in 0 .. 128 {
			b := buf[i]
			if b == 0 {
				break
			}
			out << b
		}
	}
	return out.bytestr()
}

pub fn ipc_read_request() !RpcRequest {
	mut method_buf := [128]u8{}
	mut payload_ptr := &u8(unsafe { nil })
	mut payload_len := usize(0)
	mut err_buf := [ipc_err_cap]u8{}
	rc := C.vgi_ipc_read_request_v(0, &method_buf[0], method_buf.len, &payload_ptr, &payload_len,
		&err_buf[0], ipc_err_cap)
	if rc != 0 {
		msg := unsafe { (&err_buf[0]).vstring() }
		if msg.len > 0 {
			return error('vgi_ipc_read_request: ${msg}')
		}
		return error('vgi_ipc_read_request failed (code ${rc})')
	}
	method := method_from_buf(method_buf)
	mut payload := []u8{len: int(payload_len), cap: int(payload_len)}
	if payload_len > 0 && payload_ptr != unsafe { nil } {
		unsafe {
			vmemcpy(payload.data, payload_ptr, payload_len)
		}
		C.vgi_ipc_free(payload_ptr)
	}
	return RpcRequest{
		method: method
		payload: payload
	}
}

pub fn ipc_write_raw(data []u8) ! {
	mut err_buf := [ipc_err_cap]u8{}
	rc := C.vgi_ipc_write_raw(1, data.data, data.len, &err_buf[0], ipc_err_cap)
	if rc != 0 {
		return error('vgi_ipc_write_raw failed (code ${rc})')
	}
}

pub fn ipc_write_rpc_error(error_type string, error_message string, error_kind string, schema_wire []u8) ! {
	et := error_type.bytes()
	mut et_buf := [128]u8{}
	if et.len >= et_buf.len {
		return error('error_type too long')
	}
	unsafe {
		vmemcpy(&et_buf[0], et.data, et.len)
	}
	em := error_message.bytes()
	mut em_buf := [2048]u8{}
	if em.len >= em_buf.len {
		return error('error_message too long')
	}
	unsafe {
		vmemcpy(&em_buf[0], em.data, em.len)
	}
	mut kind_ptr := &u8(unsafe { nil })
	mut kind_buf := [64]u8{}
	if error_kind.len > 0 {
		kb := error_kind.bytes()
		if kb.len >= kind_buf.len {
			return error('error_kind too long')
		}
		unsafe {
			vmemcpy(&kind_buf[0], kb.data, kb.len)
			kind_ptr = &kind_buf[0]
		}
	}
	mut err_buf := [ipc_err_cap]u8{}
	mut schema_ptr := &u8(unsafe { nil })
	schema_len := schema_wire.len
	if schema_len > 0 {
		schema_ptr = schema_wire.data
	}
	rc := C.vgi_ipc_write_rpc_error(1, &et_buf[0], &em_buf[0], kind_ptr, schema_ptr, schema_len,
		&err_buf[0], ipc_err_cap)
	if rc != 0 {
		msg := unsafe { (&err_buf[0]).vstring() }
		if msg.len > 0 {
			return error('vgi_ipc_write_rpc_error: ${msg}')
		}
		return error('vgi_ipc_write_rpc_error failed (code ${rc})')
	}
}

pub fn ipc_read_year_batch() !([]i64, []bool) {
	mut years_ptr := &i64(unsafe { nil })
	mut valid_ptr := &u8(unsafe { nil })
	mut n := i64(0)
	mut err_buf := [ipc_err_cap]u8{}
	rc := C.vgi_ipc_read_int64_column(0, &years_ptr, &valid_ptr, &n, &err_buf[0], ipc_err_cap)
	if rc == 1 {
		return []i64{}, []bool{}
	}
	if rc != 0 {
		return error('vgi_ipc_read_int64_column failed (code ${rc})')
	}
	mut years := []i64{len: int(n), cap: int(n)}
	mut valid := []bool{len: int(n), cap: int(n)}
	unsafe {
		for i in 0 .. int(n) {
			years[i] = years_ptr[i]
			valid[i] = valid_ptr[i] != 0
		}
	}
	C.vgi_ipc_free(years_ptr)
	C.vgi_ipc_free(valid_ptr)
	return years, valid
}

pub fn ipc_write_date32_result(days []i32, valid []bool) ! {
	mut mask := []u8{len: valid.len, cap: valid.len}
	for i, v in valid {
		if v {
			mask[i] = 1
		}
	}
	mut err_buf := [ipc_err_cap]u8{}
	rc := C.vgi_ipc_write_date32_result(1, days.data, mask.data, days.len, &err_buf[0], ipc_err_cap)
	if rc != 0 {
		return error('vgi_ipc_write_date32_result failed (code ${rc})')
	}
}

pub fn ipc_close_output() ! {
	mut err_buf := [ipc_err_cap]u8{}
	rc := C.vgi_ipc_close_input_stream(0, &err_buf[0], ipc_err_cap)
	if rc != 0 {
		return error('vgi_ipc_close_input_stream failed (code ${rc})')
	}
}

pub fn payload_contains(haystack []u8, needle string) bool {
	if haystack.len < needle.len {
		return false
	}
	nb := needle.bytes()
	outer := haystack.len - nb.len
	for i in 0 .. outer + 1 {
		mut ok := true
		for j in 0 .. nb.len {
			if haystack[i + j] != nb[j] {
				ok = false
				break
			}
		}
		if ok {
			return true
		}
	}
	return false
}
}
