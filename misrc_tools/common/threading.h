/*
 * MISRC Common - Platform-Agnostic Threading and Timing Primitives
 *
 * Provides a unified threading and timing API for Windows and POSIX systems.
 * For GUI code, avoids including windows.h to prevent conflicts with raylib.
 */

#ifndef MISRC_THREADING_H
#define MISRC_THREADING_H

#include <stdint.h>

#ifdef _WIN32
  #include <process.h>

  typedef void* thrd_t;
  typedef unsigned (__stdcall *thrd_start_t)(void*);

  #define thrd_success 0

  #define thrd_create(a,b,c) \
    (((*(a)=(thrd_t)_beginthreadex(NULL,0,(thrd_start_t)b,c,0,NULL))==0)?-1:thrd_success)

  /* Windows constants - define if not already defined (avoids windows.h) */
  #ifndef INFINITE
    #define INFINITE 0xFFFFFFFF
  #endif
  #ifndef WAIT_OBJECT_0
    #define WAIT_OBJECT_0 0
  #endif

  /* Thread join implementation that avoids windows.h */
  static inline int thrd_join_impl(thrd_t t, int *res) {
    /* Forward declare Windows API functions to avoid including windows.h */
    extern __declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void*, unsigned long);
    extern __declspec(dllimport) int __stdcall GetExitCodeThread(void*, unsigned long*);
    extern __declspec(dllimport) int __stdcall CloseHandle(void*);

    unsigned long exitcode = 0;
    if (WaitForSingleObject(t, INFINITE) != WAIT_OBJECT_0) return -1;
    GetExitCodeThread(t, &exitcode);
    if (res) *res = (int)exitcode;
    CloseHandle(t);
    return thrd_success;
  }
  #define thrd_join(a,b) thrd_join_impl(a,b)

  static inline void thrd_detach(thrd_t thread) {
    extern __declspec(dllimport) int __stdcall CloseHandle(void*);
    CloseHandle(thread);
  }

  /* Sleep for milliseconds */
  static inline void thrd_sleep_ms(int ms) {
    extern __declspec(dllimport) void __stdcall Sleep(unsigned long);
    Sleep((unsigned long)ms);
  }

  /* Alias for compatibility with CLI code */
  #define sleep_ms(x) thrd_sleep_ms(x)

  /* Thread priority constants */
  #define THRD_PRIORITY_NORMAL    0
  #define THRD_PRIORITY_ABOVE     1
  #define THRD_PRIORITY_HIGH      2
  #define THRD_PRIORITY_CRITICAL  3
  static inline int thrd_create_with_priority(thrd_t *thread,
                                              int (*func)(void *),
                                              void *arg,
                                              int priority) {
    (void)priority;
    return (((*thread = (thrd_t)_beginthreadex(NULL, 0, (thrd_start_t)func, arg, 0, NULL)) == 0)
            ? -1
            : thrd_success);
  }

  /* Set thread priority for current thread */
  static inline void thrd_set_priority(int priority) {
    extern __declspec(dllimport) void* __stdcall GetCurrentThread(void);
    extern __declspec(dllimport) int __stdcall SetThreadPriority(void*, int);
    /* Windows thread priorities: NORMAL=0, ABOVE_NORMAL=1, HIGHEST=2, TIME_CRITICAL=15 */
    int winprio = 0;  /* THREAD_PRIORITY_NORMAL */
    if (priority == THRD_PRIORITY_ABOVE) winprio = 1;          /* THREAD_PRIORITY_ABOVE_NORMAL */
    else if (priority == THRD_PRIORITY_HIGH) winprio = 2;      /* THREAD_PRIORITY_HIGHEST */
    else if (priority >= THRD_PRIORITY_CRITICAL) winprio = 15; /* THREAD_PRIORITY_TIME_CRITICAL */
    SetThreadPriority(GetCurrentThread(), winprio);
  }

  /* Process priority constants */
  #define PROC_PRIORITY_NORMAL      0
  #define PROC_PRIORITY_ABOVE       1
  #define PROC_PRIORITY_HIGH        2
  #define PROC_PRIORITY_CRITICAL    3

  /* Set process priority class - affects all threads including library-spawned ones */
  static inline void proc_set_priority(int priority) {
    extern __declspec(dllimport) void* __stdcall GetCurrentProcess(void);
    extern __declspec(dllimport) int __stdcall SetPriorityClass(void*, unsigned long);
    /* Windows priority classes */
    unsigned long prio_class = 0x00000020;  /* NORMAL_PRIORITY_CLASS */
    if (priority == PROC_PRIORITY_ABOVE) prio_class = 0x00008000;          /* ABOVE_NORMAL_PRIORITY_CLASS */
    else if (priority == PROC_PRIORITY_HIGH) prio_class = 0x00000080;      /* HIGH_PRIORITY_CLASS */
    else if (priority >= PROC_PRIORITY_CRITICAL) prio_class = 0x00000100;  /* REALTIME_PRIORITY_CLASS */
    SetPriorityClass(GetCurrentProcess(), prio_class);
  }

  /* Get current time in milliseconds (for timeouts, elapsed time tracking) */
  static inline uint64_t get_time_ms(void) {
    extern __declspec(dllimport) unsigned long __stdcall GetTickCount(void);
    return (uint64_t)GetTickCount();
  }

  /* Get current time in microseconds (for performance measurements) */
  static inline uint64_t get_time_us(void) {
    extern __declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);
    return (uint64_t)GetTickCount64() * 1000ULL;
  }

#else
  /* POSIX implementation */
  #include <pthread.h>
  #include <sched.h>
  #include <errno.h>
  #include <stdio.h>
  #include <stdlib.h>
  #include <sys/resource.h>
  #include <sys/types.h>
  #include <time.h>
  #include <sys/time.h>
  #include <unistd.h>
#if defined(__APPLE__)
  #include <pthread/qos.h>
  #include <signal.h>
  #include <mach/mach.h>
  #include <mach/mach_time.h>
  #include <mach/thread_policy.h>
  #include <mach/task_policy.h>
#endif
#if defined(__linux__)
  #include <dirent.h>
  #include <sys/syscall.h>
#endif

  typedef pthread_t thrd_t;

  #define thrd_success 0

  #define thrd_create(a,b,c) pthread_create(a,NULL,(void* (*)(void *))b,c)
  #define thrd_join(a,b) pthread_join(a,(void**)(b))
  #define thrd_detach(thread) pthread_detach(thread)

  /* Sleep for milliseconds */
  static inline void thrd_sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
  }

  /* Alias for compatibility with CLI code */
  #define sleep_ms(x) usleep((x)*1000)

  /* Thread priority constants */
  #define THRD_PRIORITY_NORMAL    0
  #define THRD_PRIORITY_ABOVE     1
  #define THRD_PRIORITY_HIGH      2
  #define THRD_PRIORITY_CRITICAL  3
  static inline int thrd_create_with_priority(thrd_t *thread,
                                              int (*func)(void *),
                                              void *arg,
                                              int priority) {
#if defined(__APPLE__)
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0) {
      return rc;
    }

    qos_class_t qos = QOS_CLASS_DEFAULT;
    int relpri = 0;
    int set_qos = 0;
#if defined(__arm64__) || defined(__aarch64__)
    if (priority >= THRD_PRIORITY_ABOVE) {
      qos = QOS_CLASS_USER_INTERACTIVE;
      relpri = 0;
      set_qos = 1;
    }
#else
    if (priority >= THRD_PRIORITY_CRITICAL) {
      qos = QOS_CLASS_USER_INTERACTIVE;
      relpri = 0;
      set_qos = 1;
    } else if (priority >= THRD_PRIORITY_HIGH) {
      qos = QOS_CLASS_USER_INTERACTIVE;
      relpri = -4;
      set_qos = 1;
    } else if (priority >= THRD_PRIORITY_ABOVE) {
      qos = QOS_CLASS_USER_INITIATED;
      relpri = 0;
      set_qos = 1;
    }
#endif
    if (set_qos) {
      (void)pthread_attr_set_qos_class_np(&attr, qos, relpri);
    }

    rc = pthread_create(thread, &attr, (void* (*)(void *))func, arg);
    (void)pthread_attr_destroy(&attr);
    return rc;
#else
    (void)priority;
    return thrd_create(thread, func, arg);
#endif
  }

  /* Set absolute nice value for current thread on Linux, otherwise process-wide fallback. */
  static inline int thrd_set_nice_target(int nice_target) {
#if defined(__linux__)
    pid_t tid = 0;
#if defined(SYS_gettid)
    tid = (pid_t)syscall(SYS_gettid);
#elif defined(__NR_gettid)
    tid = (pid_t)syscall(__NR_gettid);
#endif
    if (tid > 0) {
      return setpriority(PRIO_PROCESS, (id_t)tid, nice_target);
    }
#endif
    return setpriority(PRIO_PROCESS, 0, nice_target);
  }

  static inline int thrd_priority_to_nice_target(int priority) {
    if (priority == THRD_PRIORITY_ABOVE) return -5;
    if (priority == THRD_PRIORITY_HIGH) return -10;
    if (priority >= THRD_PRIORITY_CRITICAL) return -15;
    return 0;
  }

  static inline void thrd_log_priority_failure_once(const char *scope,
                                                    int priority,
                                                    int rt_err,
                                                    int nice_err) {
    static int warned_once = 0;
    if (warned_once) return;
    warned_once = 1;
    fprintf(stderr,
            "[THREAD] %s priority request %d could not be elevated (rt_err=%d, nice_err=%d)\n",
            scope, priority, rt_err, nice_err);
  }

  /* Try realtime scheduler for this thread (best-effort, usually requires elevated privileges). */
  static inline int thrd_try_realtime(int priority) {
#if defined(SCHED_FIFO) && defined(SCHED_RR)
    int policy = (priority >= THRD_PRIORITY_CRITICAL) ? SCHED_FIFO : SCHED_RR;
    int min_prio = sched_get_priority_min(policy);
    int max_prio = sched_get_priority_max(policy);
    if (min_prio < 0 || max_prio < min_prio) {
      return ENOTSUP;
    }

    struct sched_param param;
    if (priority >= THRD_PRIORITY_CRITICAL) {
      param.sched_priority = max_prio;
    } else {
      param.sched_priority = min_prio + ((max_prio - min_prio) * 3) / 4;
    }

    int rc = pthread_setschedparam(pthread_self(), policy, &param);
    if (rc == 0) {
      return 0;
    }

#if defined(__linux__)
    pid_t tid = 0;
#if defined(SYS_gettid)
    tid = (pid_t)syscall(SYS_gettid);
#elif defined(__NR_gettid)
    tid = (pid_t)syscall(__NR_gettid);
#endif
    if (tid > 0 && sched_setscheduler(tid, policy, &param) == 0) {
      return 0;
    }
    if (errno != 0) {
      return errno;
    }
#endif

    return (rc > 0) ? rc : EINVAL;
#else
    (void)priority;
    return ENOTSUP;
#endif
  }

#if defined(__linux__)
  static inline int proc_set_nice_all_threads(int nice_target) {
    DIR *task_dir = opendir("/proc/self/task");
    if (!task_dir) {
      return setpriority(PRIO_PROCESS, 0, nice_target);
    }

    int first_err = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(task_dir)) != NULL) {
      char *end = NULL;
      long tid = strtol(entry->d_name, &end, 10);
      if (end == entry->d_name || (end && *end != '\0') || tid <= 0) {
        continue;
      }
      if (setpriority(PRIO_PROCESS, (id_t)tid, nice_target) != 0 && first_err == 0) {
        first_err = (errno != 0) ? errno : EINVAL;
      }
    }
    closedir(task_dir);

    if (first_err != 0) {
      errno = first_err;
      return -1;
    }
    return 0;
  }
#endif
#if defined(__APPLE__)
  static inline void thrd_set_precedence_current_thread(integer_t importance) {
    thread_t thread = mach_thread_self();
    if (thread == MACH_PORT_NULL) {
      return;
    }
    thread_precedence_policy_data_t precedence;
    precedence.importance = importance;
    (void)thread_policy_set(thread,
                            THREAD_PRECEDENCE_POLICY,
                            (thread_policy_t)&precedence,
                            THREAD_PRECEDENCE_POLICY_COUNT);
    (void)mach_port_deallocate(mach_task_self(), thread);
  }

  /* Make the current thread non-timeshare (fixed priority). On Apple Silicon
   * this strongly biases the scheduler toward P-cluster placement, because
   * timeshare threads are the class that the CLPC scheduler happily migrates
   * onto E-cores when utilisation looks "low". */
  static inline void thrd_set_non_timeshare_current_thread(void) {
    thread_t thread = mach_thread_self();
    if (thread == MACH_PORT_NULL) {
      return;
    }
    thread_extended_policy_data_t ep;
    ep.timeshare = 0;
    (void)thread_policy_set(thread,
                            THREAD_EXTENDED_POLICY,
                            (thread_policy_t)&ep,
                            THREAD_EXTENDED_POLICY_COUNT);
    (void)mach_port_deallocate(mach_task_self(), thread);
  }

  /* Mark the current task as a foreground application. Without this the
   * kernel may classify the process (for example, children of osascript
   * "do shell script ... with administrator privileges") as background and
   * bias every thread to the efficiency cluster regardless of QoS. */
  static inline void task_set_foreground_role(void) {
    struct task_category_policy policy;
    policy.role = TASK_FOREGROUND_APPLICATION;
    (void)task_policy_set(mach_task_self(),
                          TASK_CATEGORY_POLICY,
                          (task_policy_t)&policy,
                          TASK_CATEGORY_POLICY_COUNT);
  }

  /* Explicitly clear any inherited Darwin background/throttled state that
   * would otherwise pin every thread of this task to E-cores. */
  static inline void task_clear_darwin_background(void) {
    /* PRIO_DARWIN_PROCESS / PRIO_DARWIN_BG are the documented knobs for this. */
#ifdef PRIO_DARWIN_PROCESS
    (void)setpriority(PRIO_DARWIN_PROCESS, 0, 0);
#endif
#ifdef PRIO_DARWIN_THREAD
    (void)setpriority(PRIO_DARWIN_THREAD, 0, 0);
#endif
  }

  /* One-shot startup hook for macOS processes: raise process role to
   * foreground, clear any inherited darwin-bg bit, and hint the caller
   * thread toward P-cores. Safe to call from main() before any capture
   * threads are created. */
  static inline void macos_process_prefer_p_cores(void) {
    task_set_foreground_role();
    task_clear_darwin_background();
    /* Caller thread (typically main) hint: user-interactive QoS biases
     * launched children/workers toward the performance cluster on M-series. */
    (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    thrd_set_precedence_current_thread(63);
  }

  /* Walk every thread currently in this task and force it off the timeshare
   * class with a USER_INTERACTIVE QoS override. This is the hammer we use to
   * catch threads that were spawned inside third-party libraries (libusb's
   * USB event thread, libuvc's transfer worker, raylib/Metal render helpers,
   * GCD workqueue threads, etc.) and that never route through our own
   * thrd_create_with_priority() helper. Without this, those threads remain
   * timeshare-class and the Apple Silicon CLPC scheduler migrates them onto
   * the efficiency cluster, pegging the E-cores and starving the P-core
   * capture pipeline.
   *
   * Safe to call repeatedly; new threads that appear after each call (for
   * example when hsdaoh_start_stream() spawns its transport thread) are
   * caught on the next call.
   *
   * QoS overrides are tracked per pthread and reused across sweeps so runtime
   * maintenance calls do not accumulate duplicate override tokens. */
  static inline void macos_promote_all_task_threads(void) {
    /* Track QoS overrides by pthread identity so this function can be called
     * repeatedly (runtime maintenance sweeps) without leaking a new override
     * token on every pass. */
    enum { MISRC_QOS_OVERRIDE_MAX = 1024 };
    typedef struct {
      pthread_t thread;
      pthread_override_t override_token;
    } misrc_qos_override_entry_t;
    static misrc_qos_override_entry_t s_qos_overrides[MISRC_QOS_OVERRIDE_MAX];
    static size_t s_qos_override_count = 0;
    static int s_qos_override_capacity_warned = 0;

    /* Reclaim entries for threads that no longer exist (pthread IDs may be reused). */
    for (size_t i = 0; i < s_qos_override_count;) {
      int alive_rc = pthread_kill(s_qos_overrides[i].thread, 0);
      if (alive_rc == ESRCH) {
        if (s_qos_overrides[i].override_token) {
          (void)pthread_override_qos_class_end_np(s_qos_overrides[i].override_token);
        }
        s_qos_overrides[i] = s_qos_overrides[s_qos_override_count - 1];
        s_qos_override_count--;
        continue;
      }
      i++;
    }
    thread_act_array_t threads = NULL;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
      return;
    }
    for (mach_msg_type_number_t i = 0; i < count; i++) {
      thread_act_t t = threads[i];

      /* 1) Move thread out of timeshare class. On Apple Silicon this alone
       *    stops the scheduler from migrating the thread to E-cores under
       *    low-utilisation heuristics. */
      thread_extended_policy_data_t ep;
      ep.timeshare = 0;
      (void)thread_policy_set(t,
                              THREAD_EXTENDED_POLICY,
                              (thread_policy_t)&ep,
                              THREAD_EXTENDED_POLICY_COUNT);

      /* 2) Raise precedence to match our promoted workers. */
      thread_precedence_policy_data_t pp;
      pp.importance = 63;
      (void)thread_policy_set(t,
                              THREAD_PRECEDENCE_POLICY,
                              (thread_policy_t)&pp,
                              THREAD_PRECEDENCE_POLICY_COUNT);

      /* 3) Apply an asymmetric USER_INTERACTIVE QoS override to the thread
       *    via the pthread port, if we can resolve one. This is the knob
       *    that biases external-library threads onto the P-cluster. */
      pthread_t pth = pthread_from_mach_thread_np(t);
      if (pth != NULL) {
        int already_overridden = 0;
        for (size_t j = 0; j < s_qos_override_count; j++) {
          if (pthread_equal(s_qos_overrides[j].thread, pth)) {
            already_overridden = 1;
            break;
          }
        }
        if (!already_overridden) {
          pthread_override_t override_token =
              pthread_override_qos_class_start_np(pth,
                                                  QOS_CLASS_USER_INTERACTIVE,
                                                  0);
          if (override_token) {
            if (s_qos_override_count < MISRC_QOS_OVERRIDE_MAX) {
              s_qos_overrides[s_qos_override_count].thread = pth;
              s_qos_overrides[s_qos_override_count].override_token = override_token;
              s_qos_override_count++;
            } else {
              /* Table full; avoid leaking this token. */
              (void)pthread_override_qos_class_end_np(override_token);
              if (!s_qos_override_capacity_warned) {
                s_qos_override_capacity_warned = 1;
                fprintf(stderr,
                        "[THREAD] macOS QoS override table full; skipping additional overrides\n");
              }
            }
          }
        }
      }

      (void)mach_port_deallocate(mach_task_self(), t);
    }
    (void)vm_deallocate(mach_task_self(),
                        (vm_address_t)threads,
                        count * sizeof(thread_act_t));
  }
#if defined(__arm64__) || defined(__aarch64__)
  static inline void thrd_set_time_constraint_current_thread(void) {
    static int warned_once = 0;
    thread_t thread = mach_thread_self();
    if (thread == MACH_PORT_NULL) {
      return;
    }

    mach_timebase_info_data_t timebase;
    kern_return_t tb_rc = mach_timebase_info(&timebase);
    if (tb_rc != KERN_SUCCESS || timebase.numer == 0) {
      if (!warned_once) {
        warned_once = 1;
        fprintf(stderr,
                "[THREAD] macOS time-constraint setup failed (timebase_rc=%d)\n",
                (int)tb_rc);
      }
      (void)mach_port_deallocate(mach_task_self(), thread);
      return;
    }

    const uint64_t period_ns = 1000000ULL;
    const uint64_t computation_ns = 250000ULL;
    const uint64_t constraint_ns = 900000ULL;

    uint64_t period_abs = (period_ns * (uint64_t)timebase.denom) / (uint64_t)timebase.numer;
    uint64_t computation_abs = (computation_ns * (uint64_t)timebase.denom) / (uint64_t)timebase.numer;
    uint64_t constraint_abs = (constraint_ns * (uint64_t)timebase.denom) / (uint64_t)timebase.numer;
    if (period_abs == 0) period_abs = 1;
    if (computation_abs == 0) computation_abs = 1;
    if (constraint_abs < computation_abs) constraint_abs = computation_abs;

    thread_time_constraint_policy_data_t tc_policy;
    tc_policy.period = (uint32_t)period_abs;
    tc_policy.computation = (uint32_t)computation_abs;
    tc_policy.constraint = (uint32_t)constraint_abs;
    tc_policy.preemptible = 1;
    kern_return_t tc_rc = thread_policy_set(thread,
                                            THREAD_TIME_CONSTRAINT_POLICY,
                                            (thread_policy_t)&tc_policy,
                                            THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (tc_rc != KERN_SUCCESS && !warned_once) {
      warned_once = 1;
      fprintf(stderr,
              "[THREAD] macOS time-constraint request failed (kr=%d)\n",
              (int)tc_rc);
    }

    (void)mach_port_deallocate(mach_task_self(), thread);
  }
#endif
#endif

  /* Set thread priority for current thread with realtime->nice fallback (best-effort). */
  static inline void thrd_set_priority(int priority) {
#if defined(__APPLE__)
    /* macOS: use per-thread QoS classes as the primary priority control. */
    qos_class_t qos = QOS_CLASS_DEFAULT;
    int relpri = 0;
    integer_t precedence = 0;
#if defined(__arm64__) || defined(__aarch64__)
    if (priority >= THRD_PRIORITY_ABOVE) {
      qos = QOS_CLASS_USER_INTERACTIVE;
      if (priority >= THRD_PRIORITY_CRITICAL) {
        relpri = 0;
        precedence = 63;
      } else if (priority >= THRD_PRIORITY_HIGH) {
        relpri = 0;
        precedence = 63;
      } else {
        relpri = 0;
        precedence = 47;
      }
    }
#else
    if (priority >= THRD_PRIORITY_CRITICAL) {
      qos = QOS_CLASS_USER_INTERACTIVE;
      relpri = 0;
      precedence = 63;
    } else if (priority >= THRD_PRIORITY_HIGH) {
      qos = QOS_CLASS_USER_INTERACTIVE;
      relpri = -4;
      precedence = 47;
    } else if (priority >= THRD_PRIORITY_ABOVE) {
      qos = QOS_CLASS_USER_INITIATED;
      relpri = 0;
      precedence = 31;
    }
#endif
    int qos_rc = pthread_set_qos_class_self_np(qos, relpri);
    thrd_set_precedence_current_thread(precedence);
#if defined(__arm64__) || defined(__aarch64__)
    /* On Apple Silicon, move capture-critical threads out of the timeshare
     * class so the CLPC scheduler stops migrating them onto the E-cluster
     * when instantaneous utilisation looks low (USB callback threads spend
     * most wall time blocked on the transport). Apply for ABOVE and up. */
    if (priority >= THRD_PRIORITY_ABOVE) {
      thrd_set_non_timeshare_current_thread();
    }
    if (priority >= THRD_PRIORITY_CRITICAL && qos_rc == 0) {
      thrd_set_time_constraint_current_thread();
    }
#endif
    if (qos_rc == 0) return;
    if (priority >= THRD_PRIORITY_ABOVE) {
      thrd_log_priority_failure_once("Thread QoS", priority, qos_rc, 0);
    }
#endif
    int rt_err = 0;
    int nice_err = 0;

    if (priority >= THRD_PRIORITY_HIGH) {
      rt_err = thrd_try_realtime(priority);
      if (rt_err == 0) return;
    }

    if (thrd_set_nice_target(thrd_priority_to_nice_target(priority)) == 0) {
      return;
    }

    nice_err = (errno != 0) ? errno : EINVAL;
    if (priority >= THRD_PRIORITY_ABOVE) {
      thrd_log_priority_failure_once("Thread", priority, rt_err, nice_err);
    }
  }

  /* Process priority constants */
  #define PROC_PRIORITY_NORMAL      0
  #define PROC_PRIORITY_ABOVE       1
  #define PROC_PRIORITY_HIGH        2
  #define PROC_PRIORITY_CRITICAL    3

  /* Set process priority with realtime->nice fallback (best-effort). */
  static inline void proc_set_priority(int priority) {
    int rt_err = 0;
    int nice_err = 0;
#if defined(__APPLE__)
    qos_class_t qos = QOS_CLASS_DEFAULT;
    int relpri = 0;
    integer_t caller_precedence = 0;
#if defined(__arm64__) || defined(__aarch64__)
    if (priority >= PROC_PRIORITY_ABOVE) {
      qos = QOS_CLASS_USER_INTERACTIVE;
      if (priority >= PROC_PRIORITY_CRITICAL) {
        relpri = 0;
        caller_precedence = 63;
      } else if (priority >= PROC_PRIORITY_HIGH) {
        relpri = 0;
        caller_precedence = 63;
      } else {
        relpri = 0;
        caller_precedence = 63;
      }
    }
#else
    if (priority >= PROC_PRIORITY_CRITICAL) {
      qos = QOS_CLASS_USER_INTERACTIVE;
      relpri = 0;
      caller_precedence = 63;
    } else if (priority >= PROC_PRIORITY_HIGH) {
      qos = QOS_CLASS_USER_INTERACTIVE;
      relpri = -4;
      caller_precedence = 47;
    } else if (priority >= PROC_PRIORITY_ABOVE) {
      qos = QOS_CLASS_USER_INITIATED;
      relpri = 0;
      caller_precedence = 31;
    }
#endif
    int qos_rc = pthread_set_qos_class_self_np(qos, relpri);
    thrd_set_precedence_current_thread(caller_precedence);
#if defined(__arm64__) || defined(__aarch64__)
    if (priority >= PROC_PRIORITY_ABOVE) {
      /* Mark the caller thread non-timeshare before the time-constraint
       * attempt; this alone is enough to bias onto the P-cluster on M-series. */
      thrd_set_non_timeshare_current_thread();
    }
    if (priority >= PROC_PRIORITY_ABOVE && qos_rc == 0) {
      thrd_set_time_constraint_current_thread();
    }
#endif
    if (qos_rc == 0) return;
    if (priority >= PROC_PRIORITY_ABOVE) {
      thrd_log_priority_failure_once("Process QoS", priority, qos_rc, 0);
    }
#endif

#if defined(__linux__) && defined(SCHED_FIFO) && defined(SCHED_RR)
    if (priority >= PROC_PRIORITY_HIGH) {
      int policy = (priority >= PROC_PRIORITY_CRITICAL) ? SCHED_FIFO : SCHED_RR;
      int min_prio = sched_get_priority_min(policy);
      int max_prio = sched_get_priority_max(policy);
      if (min_prio >= 0 && max_prio >= min_prio) {
        struct sched_param param;
        param.sched_priority = (priority >= PROC_PRIORITY_CRITICAL)
                               ? max_prio
                               : (min_prio + ((max_prio - min_prio) * 3) / 4);
        if (sched_setscheduler(0, policy, &param) == 0) return;
        rt_err = (errno != 0) ? errno : EINVAL;
      } else {
        rt_err = ENOTSUP;
      }
    }
#endif

    int nice_target = 0;
    if (priority == PROC_PRIORITY_ABOVE) nice_target = -5;
    else if (priority == PROC_PRIORITY_HIGH) nice_target = -10;
    else if (priority >= PROC_PRIORITY_CRITICAL) nice_target = -15;

#if defined(__linux__)
    if (proc_set_nice_all_threads(nice_target) == 0) return;
#else
    if (setpriority(PRIO_PROCESS, 0, nice_target) == 0) return;
#endif
    nice_err = (errno != 0) ? errno : EINVAL;
    if (priority >= PROC_PRIORITY_ABOVE) {
      thrd_log_priority_failure_once("Process", priority, rt_err, nice_err);
    }
  }

  /* Get current time in milliseconds (for timeouts, elapsed time tracking) */
  static inline uint64_t get_time_ms(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
      return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    }
#endif
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
  }

  /* Get current time in microseconds (for performance measurements) */
  static inline uint64_t get_time_us(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
      return (uint64_t)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
    }
#endif
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec * 1000000 + tv.tv_usec);
  }

#endif

#endif /* MISRC_THREADING_H */
