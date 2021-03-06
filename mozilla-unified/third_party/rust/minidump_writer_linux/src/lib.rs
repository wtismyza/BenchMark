use std::error;
use std::result;

type Error = Box<dyn error::Error + std::marker::Send + std::marker::Sync>;
pub type Result<T> = result::Result<T, Error>;

pub mod app_memory;
mod auxv_reader;
pub mod cpu_set;
pub mod crash_context;
mod dso_debug;
mod dumper_cpu_info;
pub mod linux_ptrace_dumper;
pub mod maps_reader;
pub mod minidump_cpu;
pub mod minidump_format;
pub mod minidump_writer;
mod sections;
pub mod thread_info;

pub use maps_reader::LINUX_GATE_LIBRARY_NAME;
