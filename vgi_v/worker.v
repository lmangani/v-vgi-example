module vgi_v

$if !test {

import os

const function_needle = 'easter_date'

fn debug(msg string) {
	if os.getenv('VGI_DEBUG') == '' {
		return
	}
	eprintln('[vgi] ${msg}')
	log_path := os.getenv('VGI_DEBUG_LOG')
	if log_path != '' {
		mut f := os.open_append(log_path) or {
			eprintln('[vgi] cannot open log ${log_path}: ${err.msg()}')
			return
		}
		defer { f.close() }
		f.writeln(msg) or {}
	}
}

pub fn serve_stdio() {
	for {
		req := ipc_read_request() or {
			debug('ipc_read_request: ${err.msg()}')
			break
		}
		debug('request method=${req.method} payload_len=${req.payload.len}')
		dispatch(req.method, req.payload) or {
			debug('dispatch ${req.method}: ${err.msg()}')
			write_dispatch_error(req.method, err) or {
				eprintln('write_dispatch_error: ${err.msg()}')
			}
		}
	}
}

fn error_schema_wire(method string) []u8 {
	return match method {
		'bind' { bind_response_wire_bin }
		'init', 'exchange' { init_header_wire_bin }
		'catalog_catalogs' { catalog_catalogs_wire_bin }
		'catalog_attach' { catalog_catalog_attach_wire_bin }
		'catalog_schemas' { catalog_catalog_schemas_wire_bin }
		'catalog_schema_contents_schemas' { catalog_catalog_schema_contents_schemas_wire_bin }
		'catalog_schema_contents_functions' { catalog_catalog_schema_contents_functions_wire_bin }
		else { []u8{} }
	}
}

fn write_dispatch_error(method string, err IError) ! {
	msg := err.msg()
	mut kind := ''
	if msg.contains('unknown method') {
		kind = 'method_not_implemented'
	}
	schema_wire := error_schema_wire(method.trim_space())
	ipc_write_rpc_error('Error', msg, kind, schema_wire)!
}

fn dispatch(method string, payload []u8) ! {
	m := method.trim_space()
	if m == 'bind' {
		handle_bind(payload)!
	} else if m == 'init' || m == 'exchange' {
		handle_init(payload)!
	} else if m == 'catalog_catalogs' {
		ipc_write_raw(catalog_catalogs_wire_bin)!
	} else if m == 'catalog_attach' {
		if !payload_contains(payload, 'easter') {
			return error('catalog_attach: unknown catalog')
		}
		ipc_write_raw(catalog_catalog_attach_wire_bin)!
	} else if m == 'catalog_schemas' {
		ipc_write_raw(catalog_catalog_schemas_wire_bin)!
	} else if m == 'catalog_schema_contents_schemas' {
		ipc_write_raw(catalog_catalog_schema_contents_schemas_wire_bin)!
	} else if m == 'catalog_schema_contents_functions' {
		ipc_write_raw(catalog_catalog_schema_contents_functions_wire_bin)!
	} else {
		return error('unknown method: "${m}" (len=${m.len})')
	}
}

fn handle_bind(_payload []u8) ! {
	ipc_write_raw(bind_response_wire_bin)!
}

fn handle_init(_payload []u8) ! {
	ipc_write_raw(init_header_wire_bin)!
	for {
		years, valid := ipc_read_year_batch()!
		if years.len == 0 {
			break
		}
		mut days := []i32{len: years.len, cap: years.len}
		mut out_valid := []bool{len: years.len, cap: years.len}
		for i, year in years {
			if !valid[i] {
				out_valid[i] = false
				days[i] = 0
				continue
			}
			d := easter_sunday(int(year))!
			days[i] = date32_days(d)
			out_valid[i] = true
		}
		ipc_write_date32_result(days, out_valid)!
	}
	ipc_close_output()!
}

}
