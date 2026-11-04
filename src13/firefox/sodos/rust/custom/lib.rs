#![no_std]
#![feature(alloc_error_handler)]

extern crate alloc;

use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};

extern "C" {
    fn malloc(size: usize) -> *mut u8;
    fn free(ptr: *mut u8);
}

struct CAlloc;

unsafe impl GlobalAlloc for CAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        malloc(layout.size())
    }
    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        free(ptr);
    }
}

#[global_allocator]
static ALLOC: CAlloc = CAlloc;

#[alloc_error_handler]
fn oom(_layout: Layout) -> ! {
    loop {}
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

#[no_mangle]
pub extern "C" fn rust_add(a: i32, b: i32) -> i32 {
    a + b
}

#[no_mangle]
pub extern "C" fn rust_vec(n: i32) -> i32 {
    let mut v: Vec<i32> = Vec::new();
    for i in 0..n {
        v.push(i);
    }
    v.iter().sum()
}
