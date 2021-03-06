/*
 * System call switch table.
 *
 * DO NOT EDIT: this file is automatically generated.
 * created from; Mimiker system call name/number "master" file.
 */

static int sys_syscall(proc_t *, syscall_args_t *, register_t *);
static int sys_exit(proc_t *, exit_args_t *, register_t *);
static int sys_fork(proc_t *, void *, register_t *);
static int sys_read(proc_t *, read_args_t *, register_t *);
static int sys_write(proc_t *, write_args_t *, register_t *);
static int sys_openat(proc_t *, openat_args_t *, register_t *);
static int sys_close(proc_t *, close_args_t *, register_t *);
static int sys_lseek(proc_t *, lseek_args_t *, register_t *);
static int sys_unlinkat(proc_t *, unlinkat_args_t *, register_t *);
static int sys_getpid(proc_t *, void *, register_t *);
static int sys_kill(proc_t *, kill_args_t *, register_t *);
static int sys_fstat(proc_t *, fstat_args_t *, register_t *);
static int sys_sbrk(proc_t *, sbrk_args_t *, register_t *);
static int sys_mmap(proc_t *, mmap_args_t *, register_t *);
static int sys_mount(proc_t *, mount_args_t *, register_t *);
static int sys_getdents(proc_t *, getdents_args_t *, register_t *);
static int sys_dup(proc_t *, dup_args_t *, register_t *);
static int sys_dup2(proc_t *, dup2_args_t *, register_t *);
static int sys_sigaction(proc_t *, sigaction_args_t *, register_t *);
static int sys_sigreturn(proc_t *, sigreturn_args_t *, register_t *);
static int sys_wait4(proc_t *, wait4_args_t *, register_t *);
static int sys_mkdirat(proc_t *, mkdirat_args_t *, register_t *);
static int sys_symlinkat(proc_t *, symlinkat_args_t *, register_t *);
static int sys_faccessat(proc_t *, faccessat_args_t *, register_t *);
static int sys_fstatat(proc_t *, fstatat_args_t *, register_t *);
static int sys_pipe2(proc_t *, pipe2_args_t *, register_t *);
static int sys_clock_gettime(proc_t *, clock_gettime_args_t *, register_t *);
static int sys_clock_nanosleep(proc_t *, clock_nanosleep_args_t *, register_t *);
static int sys_execve(proc_t *, execve_args_t *, register_t *);
static int sys_getppid(proc_t *, void *, register_t *);
static int sys_setpgid(proc_t *, setpgid_args_t *, register_t *);
static int sys_getpgid(proc_t *, getpgid_args_t *, register_t *);
static int sys_umask(proc_t *, umask_args_t *, register_t *);
static int sys_munmap(proc_t *, munmap_args_t *, register_t *);
static int sys_mprotect(proc_t *, mprotect_args_t *, register_t *);
static int sys_chdir(proc_t *, chdir_args_t *, register_t *);
static int sys_getcwd(proc_t *, getcwd_args_t *, register_t *);
static int sys_sigaltstack(proc_t *, sigaltstack_args_t *, register_t *);
static int sys_sigprocmask(proc_t *, sigprocmask_args_t *, register_t *);
static int sys_setcontext(proc_t *, setcontext_args_t *, register_t *);
static int sys_ioctl(proc_t *, ioctl_args_t *, register_t *);
static int sys_getuid(proc_t *, void *, register_t *);
static int sys_geteuid(proc_t *, void *, register_t *);
static int sys_getgid(proc_t *, void *, register_t *);
static int sys_getegid(proc_t *, void *, register_t *);
static int sys_issetugid(proc_t *, void *, register_t *);
static int sys_fcntl(proc_t *, fcntl_args_t *, register_t *);
static int sys_truncate(proc_t *, truncate_args_t *, register_t *);
static int sys_ftruncate(proc_t *, ftruncate_args_t *, register_t *);
static int sys_readlinkat(proc_t *, readlinkat_args_t *, register_t *);
static int sys_fchdir(proc_t *, fchdir_args_t *, register_t *);
static int sys_linkat(proc_t *, linkat_args_t *, register_t *);
static int sys_fchmod(proc_t *, fchmod_args_t *, register_t *);
static int sys_fchmodat(proc_t *, fchmodat_args_t *, register_t *);
static int sys_sched_yield(proc_t *, void *, register_t *);

struct sysent sysent[] = {
  [SYS_syscall] = { .nargs = 1, .call = (syscall_t *)sys_syscall },
  [SYS_exit] = { .nargs = 1, .call = (syscall_t *)sys_exit },
  [SYS_fork] = { .nargs = 0, .call = (syscall_t *)sys_fork },
  [SYS_read] = { .nargs = 3, .call = (syscall_t *)sys_read },
  [SYS_write] = { .nargs = 3, .call = (syscall_t *)sys_write },
  [SYS_openat] = { .nargs = 4, .call = (syscall_t *)sys_openat },
  [SYS_close] = { .nargs = 1, .call = (syscall_t *)sys_close },
  [SYS_lseek] = { .nargs = 3, .call = (syscall_t *)sys_lseek },
  [SYS_unlinkat] = { .nargs = 3, .call = (syscall_t *)sys_unlinkat },
  [SYS_getpid] = { .nargs = 0, .call = (syscall_t *)sys_getpid },
  [SYS_kill] = { .nargs = 2, .call = (syscall_t *)sys_kill },
  [SYS_fstat] = { .nargs = 2, .call = (syscall_t *)sys_fstat },
  [SYS_sbrk] = { .nargs = 1, .call = (syscall_t *)sys_sbrk },
  [SYS_mmap] = { .nargs = 6, .call = (syscall_t *)sys_mmap },
  [SYS_mount] = { .nargs = 2, .call = (syscall_t *)sys_mount },
  [SYS_getdents] = { .nargs = 3, .call = (syscall_t *)sys_getdents },
  [SYS_dup] = { .nargs = 1, .call = (syscall_t *)sys_dup },
  [SYS_dup2] = { .nargs = 2, .call = (syscall_t *)sys_dup2 },
  [SYS_sigaction] = { .nargs = 3, .call = (syscall_t *)sys_sigaction },
  [SYS_sigreturn] = { .nargs = 1, .call = (syscall_t *)sys_sigreturn },
  [SYS_wait4] = { .nargs = 4, .call = (syscall_t *)sys_wait4 },
  [SYS_mkdirat] = { .nargs = 3, .call = (syscall_t *)sys_mkdirat },
  [SYS_symlinkat] = { .nargs = 3, .call = (syscall_t *)sys_symlinkat },
  [SYS_faccessat] = { .nargs = 4, .call = (syscall_t *)sys_faccessat },
  [SYS_fstatat] = { .nargs = 4, .call = (syscall_t *)sys_fstatat },
  [SYS_pipe2] = { .nargs = 2, .call = (syscall_t *)sys_pipe2 },
  [SYS_clock_gettime] = { .nargs = 2, .call = (syscall_t *)sys_clock_gettime },
  [SYS_clock_nanosleep] = { .nargs = 4, .call = (syscall_t *)sys_clock_nanosleep },
  [SYS_execve] = { .nargs = 3, .call = (syscall_t *)sys_execve },
  [SYS_getppid] = { .nargs = 0, .call = (syscall_t *)sys_getppid },
  [SYS_setpgid] = { .nargs = 2, .call = (syscall_t *)sys_setpgid },
  [SYS_getpgid] = { .nargs = 1, .call = (syscall_t *)sys_getpgid },
  [SYS_umask] = { .nargs = 1, .call = (syscall_t *)sys_umask },
  [SYS_munmap] = { .nargs = 2, .call = (syscall_t *)sys_munmap },
  [SYS_mprotect] = { .nargs = 3, .call = (syscall_t *)sys_mprotect },
  [SYS_chdir] = { .nargs = 1, .call = (syscall_t *)sys_chdir },
  [SYS_getcwd] = { .nargs = 2, .call = (syscall_t *)sys_getcwd },
  [SYS_sigaltstack] = { .nargs = 2, .call = (syscall_t *)sys_sigaltstack },
  [SYS_sigprocmask] = { .nargs = 3, .call = (syscall_t *)sys_sigprocmask },
  [SYS_setcontext] = { .nargs = 1, .call = (syscall_t *)sys_setcontext },
  [SYS_ioctl] = { .nargs = 3, .call = (syscall_t *)sys_ioctl },
  [SYS_getuid] = { .nargs = 0, .call = (syscall_t *)sys_getuid },
  [SYS_geteuid] = { .nargs = 0, .call = (syscall_t *)sys_geteuid },
  [SYS_getgid] = { .nargs = 0, .call = (syscall_t *)sys_getgid },
  [SYS_getegid] = { .nargs = 0, .call = (syscall_t *)sys_getegid },
  [SYS_issetugid] = { .nargs = 0, .call = (syscall_t *)sys_issetugid },
  [SYS_fcntl] = { .nargs = 3, .call = (syscall_t *)sys_fcntl },
  [SYS_truncate] = { .nargs = 2, .call = (syscall_t *)sys_truncate },
  [SYS_ftruncate] = { .nargs = 2, .call = (syscall_t *)sys_ftruncate },
  [SYS_readlinkat] = { .nargs = 4, .call = (syscall_t *)sys_readlinkat },
  [SYS_fchdir] = { .nargs = 1, .call = (syscall_t *)sys_fchdir },
  [SYS_linkat] = { .nargs = 5, .call = (syscall_t *)sys_linkat },
  [SYS_fchmod] = { .nargs = 2, .call = (syscall_t *)sys_fchmod },
  [SYS_fchmodat] = { .nargs = 4, .call = (syscall_t *)sys_fchmodat },
  [SYS_sched_yield] = { .nargs = 0, .call = (syscall_t *)sys_sched_yield },
};

