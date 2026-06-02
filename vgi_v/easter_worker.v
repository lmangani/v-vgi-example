module main

$if test {
fn main() {}
} $else {
import vgi_v

#flag -I@VMODROOT/c
#flag -L@VMODROOT/c
#flag -l:libvgi_ipc.dylib
#flag -Wl,-rpath,@VMODROOT/c

fn main() {
	vgi_v.serve_stdio()
}
}
