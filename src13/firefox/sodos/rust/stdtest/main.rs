#![feature(restricted_std)]

extern "C" {
    fn k_dbg(s: *const u8, n: i32);
}

fn main() {
    unsafe { k_dbg(b"STD64: std main ok\n".as_ptr(), 19) };
    println!("STD64: std ok");
}
