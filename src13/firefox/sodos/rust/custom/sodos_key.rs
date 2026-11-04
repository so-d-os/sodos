use core::cell::UnsafeCell;

pub type Key = usize;

struct Value(UnsafeCell<*mut u8>);
unsafe impl Sync for Value {}

static VALUE: Value = Value(UnsafeCell::new(core::ptr::null_mut()));

#[inline]
pub fn create(_dtor: Option<unsafe extern "C" fn(*mut u8)>) -> Key {
    0
}

#[inline]
pub unsafe fn set(_key: Key, value: *mut u8) {
    unsafe { *VALUE.0.get() = value };
}

#[inline]
#[cfg(any(not(target_thread_local), test))]
pub unsafe fn get(_key: Key) -> *mut u8 {
    unsafe { *VALUE.0.get() }
}

#[inline]
pub unsafe fn destroy(_key: Key) {}
