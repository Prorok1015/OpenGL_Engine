#include "decoder_sandbox.h"
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <ctime>
#include <initializer_list>
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SNK_DECODER_SANITIZER_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SNK_DECODER_SANITIZER_BUILD 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define SNK_DECODER_LEAK_CHECK 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SNK_DECODER_LEAK_CHECK 1
#endif
#endif

#if defined(SNK_DECODER_LEAK_CHECK) && defined(__linux__)
#include <sanitizer/lsan_interface.h>
#endif

// Sanitizer builds install no filter: their runtime makes calls no allowlist foresees.
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__)) && !defined(SNK_DECODER_SANITIZER_BUILD)
#define SNK_DECODER_SYSCALL_FILTER 1
#endif

namespace {

#if defined(__linux__)

std::string errno_text(const char* what, int error)
{
	return std::string(what) + ": " + std::generic_category().message(error);
}

// The Landlock UAPI (<linux/landlock.h>), spelled out here: the build machine's header may predate the ABI the kernel has.
// Values from the kernel's include/uapi/linux/landlock.h; the syscall numbers are the same on x86_64 and aarch64.
namespace landlock {

#if defined(__NR_landlock_create_ruleset)
	constexpr long nr_create_ruleset = __NR_landlock_create_ruleset;
	constexpr long nr_add_rule = __NR_landlock_add_rule;
	constexpr long nr_restrict_self = __NR_landlock_restrict_self;
#else
	constexpr long nr_create_ruleset = 444;
	constexpr long nr_add_rule = 445;
	constexpr long nr_restrict_self = 446;
#endif

	constexpr unsigned create_ruleset_version = 1u << 0;
	constexpr int rule_path_beneath = 1;

	constexpr std::uint64_t fs_execute = 1ull << 0;
	constexpr std::uint64_t fs_write_file = 1ull << 1;
	constexpr std::uint64_t fs_read_file = 1ull << 2;
	constexpr std::uint64_t fs_read_dir = 1ull << 3;
	constexpr std::uint64_t fs_remove_dir = 1ull << 4;
	constexpr std::uint64_t fs_remove_file = 1ull << 5;
	constexpr std::uint64_t fs_make_char = 1ull << 6;
	constexpr std::uint64_t fs_make_dir = 1ull << 7;
	constexpr std::uint64_t fs_make_reg = 1ull << 8;
	constexpr std::uint64_t fs_make_sock = 1ull << 9;
	constexpr std::uint64_t fs_make_fifo = 1ull << 10;
	constexpr std::uint64_t fs_make_block = 1ull << 11;
	constexpr std::uint64_t fs_make_sym = 1ull << 12;
	constexpr std::uint64_t fs_refer = 1ull << 13;		// ABI 2
	constexpr std::uint64_t fs_truncate = 1ull << 14;	// ABI 3
	constexpr std::uint64_t fs_ioctl_dev = 1ull << 15;	// ABI 5

	constexpr std::uint64_t net_bind_tcp = 1ull << 0;		// ABI 4
	constexpr std::uint64_t net_connect_tcp = 1ull << 1;	// ABI 4

	constexpr std::uint64_t scope_abstract_unix_socket = 1ull << 0;	// ABI 6
	constexpr std::uint64_t scope_signal = 1ull << 1;					// ABI 6

	struct ruleset_attr
	{
		std::uint64_t handled_access_fs = 0;
		std::uint64_t handled_access_net = 0;
		std::uint64_t scoped = 0;
	};

	struct __attribute__((packed)) path_beneath_attr
	{
		std::uint64_t allowed_access = 0;
		std::int32_t parent_fd = -1;
	};

} // namespace landlock

#if defined(__NR_close_range)
constexpr long k_nr_close_range = __NR_close_range;
#else
constexpr long k_nr_close_range = 436;
#endif

struct landlock_probe
{
	int abi = 0;			// 0: absent
	std::string absent;		// why it is absent
	std::string failure;	// the probe itself failed in a way that is no absence
};

landlock_probe probe_landlock(bool pretend_absent)
{
	landlock_probe probe;
	if (pretend_absent) {
		probe.absent = "Landlock is reported absent (--self-test-pretend-no-landlock)";
		return probe;
	}
	const long abi = ::syscall(landlock::nr_create_ruleset, nullptr, std::size_t{ 0 }, landlock::create_ruleset_version);
	if (abi < 0) {
		const int error = errno;
		// EPERM: a container's own seccomp policy forbids the call; for this process Landlock is not there either.
		if (error == ENOSYS || error == EOPNOTSUPP || error == EPERM) {
			probe.absent = errno_text("the kernel has no usable Landlock", error);
		} else {
			probe.failure = errno_text("the Landlock probe failed", error);
		}
		return probe;
	}
	if (abi < 1) {
		probe.absent = "the kernel reports no Landlock ABI";
		return probe;
	}
	probe.abi = static_cast<int>(abi);
	return probe;
}

// The run folder (the working directory) is the one hierarchy the child may touch, and nothing in it may be executed.
std::string apply_landlock(int abi)
{
	using namespace landlock;
	std::uint64_t handled_fs = fs_execute | fs_write_file | fs_read_file | fs_read_dir | fs_remove_dir | fs_remove_file | fs_make_char
		| fs_make_dir | fs_make_reg | fs_make_sock | fs_make_fifo | fs_make_block | fs_make_sym;
	if (abi >= 2) handled_fs |= fs_refer;
	if (abi >= 3) handled_fs |= fs_truncate;
	if (abi >= 5) handled_fs |= fs_ioctl_dev;

	// The size tells the kernel which fields the ruleset has: an older kernel refuses fields it does not know.
	ruleset_attr attr;
	attr.handled_access_fs = handled_fs;
	std::size_t attr_size = offsetof(ruleset_attr, handled_access_net);
	if (abi >= 4) {
		// Handled with no rule: no TCP bind or connect at all.
		attr.handled_access_net = net_bind_tcp | net_connect_tcp;
		attr_size = offsetof(ruleset_attr, scoped);
	}
	if (abi >= 6) {
		attr.scoped = scope_abstract_unix_socket | scope_signal;
		attr_size = sizeof(ruleset_attr);
	}
	const long ruleset = ::syscall(nr_create_ruleset, &attr, attr_size, 0u);
	if (ruleset < 0) return errno_text("landlock_create_ruleset", errno);
	const int ruleset_fd = static_cast<int>(ruleset);

	const int scratch = ::open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (scratch < 0) {
		const int error = errno;
		::close(ruleset_fd);
		return errno_text("cannot open the run folder", error);
	}
	path_beneath_attr rule;
	rule.allowed_access = handled_fs & ~fs_execute;
	rule.parent_fd = scratch;
	const long added = ::syscall(nr_add_rule, ruleset_fd, rule_path_beneath, &rule, 0u);
	const int add_error = errno;
	::close(scratch);
	if (added != 0) {
		::close(ruleset_fd);
		return errno_text("landlock_add_rule", add_error);
	}
	const long restricted = ::syscall(nr_restrict_self, ruleset_fd, 0u);
	const int restrict_error = errno;
	::close(ruleset_fd);
	if (restricted != 0) return errno_text("landlock_restrict_self", restrict_error);
	return {};
}

#if defined(SNK_DECODER_SYSCALL_FILTER)

#if defined(__x86_64__)
constexpr std::uint32_t k_audit_arch = AUDIT_ARCH_X86_64;
#else
constexpr std::uint32_t k_audit_arch = AUDIT_ARCH_AARCH64;
#endif

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the filter reads the low half of an argument at its first bytes");

// Everything the decode path calls after the filter: reading the request and the model, the import's memory:// folder
// (./sandbox, written by the resource system's io thread through a temp file and a rename), the answer, the log, the
// io threads of the resource system (boost::asio: epoll, eventfd, timerfd; threads through clone below), libstdc++'s
// filesystem (stat, getcwd, readlink of each part through realpath) and glibc's own (rseq, robust lists, getrandom).
// x86_64 still has the old path calls (mkdir, rename, unlink...) that glibc uses there instead of the *at ones.
constexpr long k_allowed[] = {
	__NR_read, __NR_write, __NR_readv, __NR_writev, __NR_pread64, __NR_pwrite64, __NR_openat, __NR_close, __NR_fstat,
	__NR_lseek, __NR_mmap, __NR_munmap, __NR_mprotect, __NR_madvise, __NR_mremap, __NR_brk, __NR_futex,
	__NR_rt_sigaction, __NR_rt_sigprocmask, __NR_rt_sigreturn, __NR_sigaltstack, __NR_getrandom, __NR_clock_gettime,
	__NR_clock_getres, __NR_clock_nanosleep, __NR_nanosleep, __NR_gettimeofday, __NR_restart_syscall, __NR_gettid,
	__NR_getpid, __NR_getuid, __NR_geteuid, __NR_getgid, __NR_getegid, __NR_sysinfo, __NR_exit, __NR_exit_group,
	__NR_unlinkat, __NR_renameat2, __NR_mkdirat, __NR_getdents64, __NR_fcntl, __NR_dup, __NR_dup3, __NR_sched_yield,
	__NR_sched_getaffinity, __NR_set_robust_list, __NR_ftruncate, __NR_fsync, __NR_fdatasync,
	__NR_getcwd, __NR_readlinkat, __NR_faccessat, __NR_newfstatat, __NR_statx, __NR_epoll_create1, __NR_epoll_ctl,
	__NR_epoll_pwait, __NR_eventfd2, __NR_timerfd_create, __NR_timerfd_settime, __NR_timerfd_gettime,
#if defined(__NR_rseq)
	__NR_rseq,
#endif
#if defined(__NR_faccessat2)
	__NR_faccessat2,
#endif
#if defined(__NR_epoll_pwait2)
	__NR_epoll_pwait2,
#endif
#if defined(__NR_renameat)
	__NR_renameat,
#endif
#if defined(__x86_64__)
	__NR_stat, __NR_lstat, __NR_access, __NR_mkdir, __NR_rmdir, __NR_unlink, __NR_rename, __NR_readlink, __NR_epoll_wait,
	__NR_time,
#endif
};

constexpr std::uint32_t k_ret_allow = SECCOMP_RET_ALLOW;
constexpr std::uint32_t k_ret_eperm = SECCOMP_RET_ERRNO | (static_cast<std::uint32_t>(EPERM) & SECCOMP_RET_DATA);
[[maybe_unused]] constexpr std::uint32_t k_ret_enosys = SECCOMP_RET_ERRNO | (static_cast<std::uint32_t>(ENOSYS) & SECCOMP_RET_DATA);
constexpr std::uint32_t k_ret_kill = SECCOMP_RET_KILL_PROCESS;

constexpr std::uint32_t argument_low(std::size_t index)
{
	return static_cast<std::uint32_t>(offsetof(seccomp_data, args) + index * sizeof(std::uint64_t));
}

class filter_program
{
public:
	void load(std::uint32_t offset) { add(BPF_LD | BPF_W | BPF_ABS, offset, 0, 0); }
	void ret(std::uint32_t action) { add(BPF_RET | BPF_K, action, 0, 0); }
	void jump_eq(std::uint32_t value, std::uint8_t if_true, std::uint8_t if_false) { add(BPF_JMP | BPF_JEQ | BPF_K, value, if_true, if_false); }
	void jump_set(std::uint32_t bits, std::uint8_t if_true, std::uint8_t if_false) { add(BPF_JMP | BPF_JSET | BPF_K, bits, if_true, if_false); }
	void and_with(std::uint32_t mask) { add(BPF_ALU | BPF_AND | BPF_K, mask, 0, 0); }

	std::vector<sock_filter>& code() { return m_code; }

private:
	void add(int code, std::uint32_t k, std::uint8_t if_true, std::uint8_t if_false)
	{
		m_code.push_back(sock_filter{ static_cast<std::uint16_t>(code), if_true, if_false, k });
	}

	std::vector<sock_filter> m_code;
};

std::uint32_t syscall_number(long nr)
{
	return static_cast<std::uint32_t>(nr);
}

std::vector<sock_filter> filter_code()
{
	filter_program program;
	// Another ABI (x32, i386 through int 0x80) would read the numbers below as other calls.
	program.load(static_cast<std::uint32_t>(offsetof(seccomp_data, arch)));
	program.jump_eq(k_audit_arch, 1, 0);
	program.ret(k_ret_kill);
	program.load(static_cast<std::uint32_t>(offsetof(seccomp_data, nr)));
#if defined(__x86_64__)
	program.jump_set(0x40000000u, 0, 1);	// __X32_SYSCALL_BIT
	program.ret(k_ret_kill);
#endif

#if defined(__NR_clone3)
	// clone3 passes its flags in memory the filter cannot read: ENOSYS makes glibc's pthread_create fall back to clone.
	program.jump_eq(syscall_number(__NR_clone3), 0, 1);
	program.ret(k_ret_enosys);
#endif

	// clone only for a thread of this process (the resource system's io threads): never a new process or namespace.
	constexpr std::uint32_t thread_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
	constexpr std::uint32_t refused_flags = CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID
		| CLONE_NEWNET | CLONE_PTRACE | CLONE_VFORK | CLONE_UNTRACED;
	program.jump_eq(syscall_number(__NR_clone), 0, 5);
	program.load(argument_low(0));
	program.and_with(thread_flags | refused_flags);
	program.jump_eq(thread_flags, 0, 1);
	program.ret(k_ret_allow);
	program.ret(k_ret_eperm);

	// abort() signals itself: only to this process.
	const std::uint32_t self = static_cast<std::uint32_t>(::getpid());
	for (const long nr : { static_cast<long>(__NR_tgkill), static_cast<long>(__NR_tkill) }) {
		program.jump_eq(syscall_number(nr), 0, 4);
		program.load(argument_low(0));
		program.jump_eq(self, 0, 1);
		program.ret(k_ret_allow);
		program.ret(k_ret_eperm);
	}

	// prlimit64 of another process of this user would change the editor's own limits: only 0 (this process) or itself.
	program.jump_eq(syscall_number(__NR_prlimit64), 0, 5);
	program.load(argument_low(0));
	program.jump_eq(0u, 1, 0);
	program.jump_eq(self, 0, 1);
	program.ret(k_ret_allow);
	program.ret(k_ret_eperm);

	for (const long nr : k_allowed) {
		program.jump_eq(syscall_number(nr), 0, 1);
		program.ret(k_ret_allow);
	}
	program.ret(k_ret_eperm);
	return std::move(program.code());
}

std::string install_syscall_filter()
{
	std::vector<sock_filter> code = filter_code();
	const sock_fprog program{ static_cast<unsigned short>(code.size()), code.data() };
	if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program, 0, 0) != 0) return errno_text("installing the seccomp filter", errno);
	return {};
}

#endif // SNK_DECODER_SYSCALL_FILTER

// Every descriptor but stdin, stdout and stderr: nothing the editor or a library left open reaches the decode.
void close_inherited_descriptors()
{
	if (::syscall(k_nr_close_range, 3u, ~0u, 0u) == 0) return;
	rlimit open_files{};
	rlim_t last = 4096;
	if (::getrlimit(RLIMIT_NOFILE, &open_files) == 0 && open_files.rlim_cur != RLIM_INFINITY) last = std::min<rlim_t>(open_files.rlim_cur, 1u << 20);
	for (rlim_t fd = 3; fd < last; ++fd) ::close(static_cast<int>(fd));
}

#endif // __linux__

} // namespace

decoder_sandbox::status decoder_sandbox::apply(const settings& wanted)
{
	status result;
	const bool required = wanted.mode == edt::sandbox_mode::required;
#if defined(__linux__)
	if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
		result.refusal = errno_text("no_new_privs cannot be set", errno);
		return result;
	}
	close_inherited_descriptors();
	rlimit file_size{};
	if (::getrlimit(RLIMIT_FSIZE, &file_size) != 0) {
		result.refusal = errno_text("RLIMIT_FSIZE cannot be read", errno);
		return result;
	}
	const rlim_t largest = std::min<rlim_t>(static_cast<rlim_t>(wanted.max_output_bytes), file_size.rlim_max);
	file_size.rlim_cur = largest;
	file_size.rlim_max = largest;
	if (::setrlimit(RLIMIT_FSIZE, &file_size) != 0) {
		result.refusal = errno_text("RLIMIT_FSIZE cannot be set", errno);
		return result;
	}

#if defined(SNK_MODEL_DECODER_SELF_TEST)
	const landlock_probe fs_probe = probe_landlock(wanted.pretend_no_landlock);
#else
	const landlock_probe fs_probe = probe_landlock(false);
#endif
	if (!fs_probe.failure.empty()) {
		result.refusal = fs_probe.failure;
		return result;
	}

	// The filter is left out of sanitizer builds (their runtime makes calls no allowlist foresees); such a build is a test
	// build, never shipped, so `required` there is met by Landlock alone.
	std::string no_filter;
	bool filter_absent_by_build = false;
#if defined(SNK_DECODER_SANITIZER_BUILD)
	no_filter = "a sanitizer build has no system call filter";
	filter_absent_by_build = true;
#elif !defined(SNK_DECODER_SYSCALL_FILTER)
	no_filter = "there is no system call filter for this processor";
#else
	if (::prctl(PR_GET_SECCOMP, 0, 0, 0, 0) < 0 && errno == EINVAL) no_filter = "the kernel has no seccomp";
#endif

	if (required && fs_probe.abi == 0) {
		result.refusal = fs_probe.absent;
		return result;
	}
	if (required && !no_filter.empty() && !filter_absent_by_build) {
		result.refusal = no_filter;
		return result;
	}

	if (fs_probe.abi > 0) {
		// Outside the run folder from here on: the time zone is read now, the leak check of an ASan build runs now (it
		// reads /proc, which Landlock closes; after it has run once, it does not run again at exit).
		::tzset();
#if defined(SNK_DECODER_LEAK_CHECK)
		__lsan_do_leak_check();
#endif
		if (std::string failed = apply_landlock(fs_probe.abi); !failed.empty()) {
			result.refusal = std::move(failed);
			return result;
		}
		result.fs = true;
	} else {
		result.why = fs_probe.absent;
	}

#if defined(SNK_DECODER_SYSCALL_FILTER)
	if (no_filter.empty()) {
		if (std::string failed = install_syscall_filter(); !failed.empty()) {
			result.refusal = std::move(failed);
			return result;
		}
		result.syscalls = true;
	}
#endif
	if (!no_filter.empty()) result.why += (result.why.empty() ? "" : "; ") + no_filter;
#else
	result.why = "this system has no sandbox for the decoder yet (Landlock and seccomp are Linux only)";
	if (required) result.refusal = result.why;
#endif
	return result;
}

std::string decoder_sandbox::report_line(const status& applied)
{
	return std::string("fs=") + (applied.fs ? "landlock" : "none") + " syscalls=" + (applied.syscalls ? "seccomp" : "none");
}
