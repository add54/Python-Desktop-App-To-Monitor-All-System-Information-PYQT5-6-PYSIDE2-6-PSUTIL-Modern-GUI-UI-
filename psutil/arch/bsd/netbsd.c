/*
 * Copyright (c) 2009, Giampaolo Rodola', Landry Breuil.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Platform-specific module methods for NetBSD.
 */

#if defined(__NetBSD__)
#define _KMEMUSER
#endif

#include <Python.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/proc.h>
#include <sys/swap.h>  // for swap_mem
#include <signal.h>
#include <kvm.h>
// connection stuff
#include <netdb.h>  // for NI_MAXHOST
#include <sys/socket.h>
#include <sys/sched.h>  // for CPUSTATES & CP_*
#define _KERNEL  // for DTYPE_*
#include <sys/file.h>
#undef _KERNEL
#include <sys/disk.h>  // struct diskstats
#include <netinet/in.h>
#include <arpa/inet.h>


#include "netbsd.h"
#include "netbsd_socks.h"
#include "../../_psutil_common.h"

#define PSUTIL_KPT2DOUBLE(t) (t ## _sec + t ## _usec / 1000000.0)
#define PSUTIL_TV2DOUBLE(t) ((t).tv_sec + (t).tv_usec / 1000000.0)


// ============================================================================
// Utility functions
// ============================================================================


int
psutil_raise_ad_or_nsp(long pid) {
    // Set exception to AccessDenied if pid exists else NoSuchProcess.
    if (psutil_pid_exists(pid) == 0)
        NoSuchProcess();
    else
        AccessDenied();
}


int
psutil_kinfo_proc(pid_t pid, kinfo_proc *proc) {
    // Fills a kinfo_proc struct based on process pid.
    int ret;
    int mib[6];
    size_t size = sizeof(kinfo_proc);

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC2;
    mib[2] = KERN_PROC_PID;
    mib[3] = pid;
    mib[4] = size;
    mib[5] = 1;

    ret = sysctl((int*)mib, 6, proc, &size, NULL, 0);
    if (ret == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
    // sysctl stores 0 in the size if we can't find the process information.
    if (size == 0) {
        NoSuchProcess();
        return -1;
    }
    return 0;
}


struct kinfo_file *
kinfo_getfile(pid_t pid, int* cnt) {
    // Mimic's FreeBSD kinfo_file call, taking a pid and a ptr to an
    // int as arg and returns an array with cnt struct kinfo_file.
    int mib[6];
    size_t len;
    struct kinfo_file* kf;
    mib[0] = CTL_KERN;
    mib[1] = KERN_FILE2;
    mib[2] = KERN_FILE_BYPID;
    mib[3] = (int) pid;
    mib[4] = sizeof(struct kinfo_file);
    mib[5] = 0;

    // get the size of what would be returned
    if (sysctl(mib, 6, NULL, &len, NULL, 0) < 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }
    if ((kf = malloc(len)) == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    mib[5] = (int)(len / sizeof(struct kinfo_file));
    if (sysctl(mib, 6, kf, &len, NULL, 0) < 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }

    *cnt = (int)(len / sizeof(struct kinfo_file));
    return kf;
}


int
psutil_pid_exists(pid_t pid) {
    // Return 1 if PID exists in the current process list, else 0, -1
    // on error.
    // TODO: this should live in _psutil_posix.c but for some reason if I
    // move it there I get a "include undefined symbol" error.
    int ret;
    if (pid < 0)
        return 0;
    ret = kill(pid , 0);
    if (ret == 0)
        return 1;
    else {
        if (ret == ESRCH)
            return 0;
        else if (ret == EPERM)
            return 1;
        else {
            PyErr_SetFromErrno(PyExc_OSError);
            return -1;
        }
    }
}

PyObject *
psutil_proc_exe(PyObject *self, PyObject *args) {
#if __NetBSD_Version__ >= 799000000
    pid_t pid;
    char pathname[MAXPATHLEN];
    int error;
    int mib[4];
    int ret;
    size_t size;

    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC_ARGS;
    mib[2] = pid;
    mib[3] = KERN_PROC_PATHNAME;

    size = sizeof(pathname);
    error = sysctl(mib, 4, NULL, &size, NULL, 0);
    if (error == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }

    error = sysctl(mib, 4, pathname, &size, NULL, 0);
    if (error == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }
    if (size == 0 || strlen(pathname) == 0) {
        ret = psutil_pid_exists(pid);
        if (ret == -1)
            return NULL;
        else if (ret == 0)
            return NoSuchProcess();
        else
            strcpy(pathname, "");
    }
    return Py_BuildValue("s", pathname);
#else
    return Py_BuildValue("s", "");
#endif
}

PyObject *
psutil_proc_num_threads(PyObject *self, PyObject *args) {
    // Return number of threads used by process as a Python integer.
    long pid;
    kinfo_proc kp;
    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kp) == -1)
        return NULL;
    return Py_BuildValue("l", (long)kp.p_nlwps);
}

PyObject *
psutil_proc_threads(PyObject *self, PyObject *args) {
    pid_t pid;
    int mib[5];
    int i, nlwps;
    ssize_t st;
    size_t size;
    struct kinfo_lwp *kl = NULL;
    PyObject *py_retlist = PyList_New(0);
    PyObject *py_tuple = NULL;

    if (py_retlist == NULL)
        return NULL;
    if (! PyArg_ParseTuple(args, "l", &pid))
        goto error;

    mib[0] = CTL_KERN;
    mib[1] = KERN_LWP;
    mib[2] = pid;
    mib[3] = sizeof(struct kinfo_lwp);
    mib[4] = 0;

    st = sysctl(mib, 5, NULL, &size, NULL, 0);
    if (st == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto error;
    }
    if (size == 0) {
        NoSuchProcess();
        goto error;
    }

    mib[4] = size / sizeof(size_t);
    kl = malloc(size);
    if (kl == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    st = sysctl(mib, 5, kl, &size, NULL, 0);
    if (st == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto error;
    }
    if (size == 0) {
        NoSuchProcess();
        goto error;
    }

    nlwps = (int)(size / sizeof(struct kinfo_lwp));
    for (i = 0; i < nlwps; i++) {
        py_tuple = Py_BuildValue("idd",
                                 (&kl[i])->l_lid,
                                 PSUTIL_KPT2DOUBLE((&kl[i])->l_rtime),
                                 PSUTIL_KPT2DOUBLE((&kl[i])->l_rtime));
        if (py_tuple == NULL)
            goto error;
        if (PyList_Append(py_retlist, py_tuple))
            goto error;
        Py_DECREF(py_tuple);
    }
    free(kl);
    return py_retlist;

error:
    Py_XDECREF(py_tuple);
    Py_DECREF(py_retlist);
    if (kl != NULL)
        free(kl);
    return NULL;
}


// ============================================================================
// APIS
// ============================================================================

int
psutil_get_proc_list(kinfo_proc **procList, size_t *procCount) {
    // Returns a list of all BSD processes on the system.  This routine
    // allocates the list and puts it in *procList and a count of the
    // number of entries in *procCount.  You are responsible for freeing
    // this list (use "free" from System framework).
    // On success, the function returns 0.
    // On error, the function returns a BSD errno value.
    kinfo_proc *result;
    int done;
    static const int name[] = { CTL_KERN, KERN_PROC, KERN_PROC, 0 };
    // Declaring name as const requires us to cast it when passing it to
    // sysctl because the prototype doesn't include the const modifier.
    size_t length;
    char errbuf[_POSIX2_LINE_MAX];
    kinfo_proc *x;
    int cnt;
    kvm_t *kd;

    assert( procList != NULL);
    assert(*procList == NULL);
    assert(procCount != NULL);

    kd = kvm_openfiles(NULL, NULL, NULL, KVM_NO_FILES, errbuf);

    if (kd == NULL) {
        PyErr_Format(PyExc_RuntimeError, "kvm_openfiles() failed: %s", errbuf);
        return errno;
    }

    result = kvm_getproc2(kd, KERN_PROC_ALL, 0, sizeof(kinfo_proc), &cnt);
    if (result == NULL) {
        PyErr_Format(PyExc_RuntimeError, "kvm_getproc2() failed");
        kvm_close(kd);
        return errno;
    }

    *procCount = (size_t)cnt;

    size_t mlen = cnt * sizeof(kinfo_proc);

    if ((*procList = malloc(mlen)) == NULL) {
        PyErr_NoMemory();
        kvm_close(kd);
        return errno;
    }

    memcpy(*procList, result, mlen);
    assert(*procList != NULL);
    kvm_close(kd);

    return 0;
}


char *
psutil_get_cmd_args(pid_t pid, size_t *argsize) {
    int mib[4];
    ssize_t st;
    size_t argmax;
    size_t size;
    char *procargs = NULL;

    mib[0] = CTL_KERN;
    mib[1] = KERN_ARGMAX;

    size = sizeof(argmax);
    st = sysctl(mib, 2, &argmax, &size, NULL, 0);
    if (st == -1) {
        warn("failed to get kern.argmax");
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }

    procargs = (char *)malloc(argmax);
    if (procargs == NULL) {
        PyErr_NoMemory();
        return NULL;
    }

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC_ARGS;
    mib[2] = pid;
    mib[3] = KERN_PROC_ARGV;

    st = sysctl(mib, 4, procargs, &argmax, NULL, 0);
    if (st == -1) {
        warn("failed to get kern.procargs");
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }

    *argsize = argmax;
    return procargs;
}

// returns the command line as a python list object
PyObject *
psutil_get_cmdline(pid_t pid) {
    char *argstr = NULL;
    int pos = 0;
    size_t argsize = 0;
    PyObject *py_arg = NULL;
    PyObject *py_retlist = PyList_New(0);
    if (py_retlist == NULL)
        return NULL;

    argstr = psutil_get_cmd_args(pid, &argsize);
    if (argstr == NULL)
        goto error;

    // args are returned as a flattened string with \0 separators between
    // arguments add each string to the list then step forward to the next
    // separator
    if (argsize > 0) {
        while (pos < argsize) {
            py_arg = Py_BuildValue("s", &argstr[pos]);
            if (!py_arg)
                goto error;
            if (PyList_Append(py_retlist, py_arg))
                goto error;
            Py_DECREF(py_arg);
            pos = pos + strlen(&argstr[pos]) + 1;
        }
    }

    free(argstr);
    return py_retlist;

error:
    Py_XDECREF(py_arg);
    Py_DECREF(py_retlist);
    if (argstr != NULL)
        free(argstr);
    return NULL;
}


PyObject *
psutil_virtual_mem(PyObject *self, PyObject *args) {
    unsigned int total, active, inactive, wired, cached, free;
    size_t size = sizeof(total);
    struct uvmexp_sysctl uvmexp;
    int mib[] = {CTL_VM, VM_UVMEXP2};
    long pagesize = getpagesize();
    size = sizeof(uvmexp);

    if (sysctl(mib, 2, &uvmexp, &size, NULL, 0) < 0) {
        warn("failed to get vm.uvmexp");
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }
    return Py_BuildValue("KKKKKKKK",
        (unsigned long long) uvmexp.npages * pagesize,
        (unsigned long long) uvmexp.free * pagesize,
        (unsigned long long) uvmexp.active * pagesize,
        (unsigned long long) uvmexp.inactive * pagesize,
        (unsigned long long) uvmexp.wired * pagesize,
        (unsigned long long) 0,
        (unsigned long long) 0,
        (unsigned long long) 0
    );
}


PyObject *
psutil_swap_mem(PyObject *self, PyObject *args) {
    uint64_t swap_total, swap_free;
    struct swapent *swdev;
    int nswap, i;

    if ((nswap = swapctl(SWAP_NSWAP, 0, 0)) == 0) {
        warn("failed to get swap device count");
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }

    if ((swdev = calloc(nswap, sizeof(*swdev))) == NULL) {
        warn("failed to allocate memory for swdev structures");
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }

    if (swapctl(SWAP_STATS, swdev, nswap) == -1) {
        free(swdev);
        warn("failed to get swap stats");
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }

    // Total things up
    swap_total = swap_free = 0;
    for (i = 0; i < nswap; i++) {
        if (swdev[i].se_flags & SWF_ENABLE) {
            swap_free += (swdev[i].se_nblks - swdev[i].se_inuse);
            swap_total += swdev[i].se_nblks;
        }
    }
    free(swdev);
    return Py_BuildValue("(LLLII)",
                         swap_total * DEV_BSIZE,
                         (swap_total - swap_free) * DEV_BSIZE,
                         swap_free * DEV_BSIZE,
                         0, // XXX swap in
                         0); // XXX swap out
}


PyObject *
psutil_proc_num_fds(PyObject *self, PyObject *args) {
    long pid;
    int cnt;

    struct kinfo_file *freep;

    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;

    freep = kinfo_getfile(pid, &cnt);
    if (freep == NULL) {
        psutil_raise_ad_or_nsp(pid);
        return NULL;
    }
    free(freep);

    return Py_BuildValue("i", cnt);
}


PyObject *
psutil_proc_cwd(PyObject *self, PyObject *args) {
    // Not implemented
    return NULL;
}


// see sys/kern/kern_sysctl.c lines 1100 and
// usr.bin/fstat/fstat.c print_inet_details()
static char *
psutil_convert_ipv4(int family, uint32_t addr[4]) {
    struct in_addr a;
    memcpy(&a, addr, sizeof(a));
    return inet_ntoa(a);
}


static char *
psutil_inet6_addrstr(struct in6_addr *p) {
    struct sockaddr_in6 sin6;
    static char hbuf[NI_MAXHOST];
    const int niflags = NI_NUMERICHOST;

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_len = sizeof(struct sockaddr_in6);
    sin6.sin6_addr = *p;
    if (IN6_IS_ADDR_LINKLOCAL(p) &&
        *(u_int16_t *)&sin6.sin6_addr.s6_addr[2] != 0) {
        sin6.sin6_scope_id =
            ntohs(*(u_int16_t *)&sin6.sin6_addr.s6_addr[2]);
        sin6.sin6_addr.s6_addr[2] = sin6.sin6_addr.s6_addr[3] = 0;
    }

    if (getnameinfo((struct sockaddr *)&sin6, sin6.sin6_len,
        hbuf, sizeof(hbuf), NULL, 0, niflags))
        return "invalid";

    return hbuf;
}

PyObject *
psutil_per_cpu_times(PyObject *self, PyObject *args) {
    static int maxcpus;
    int mib[3];
    int ncpu;
    size_t len;
    size_t size;
    int i;
    PyObject *py_retlist = PyList_New(0);
    PyObject *py_cputime = NULL;

    if (py_retlist == NULL)
        return NULL;


    // retrieve the number of cpus
    mib[0] = CTL_HW;
    mib[1] = HW_NCPU;
    len = sizeof(ncpu);
    if (sysctl(mib, 2, &ncpu, &len, NULL, 0) == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto error;
    }
    uint64_t cpu_time[CPUSTATES];

    for (i = 0; i < ncpu; i++) {
        // per-cpu info
        mib[0] = CTL_KERN;
        mib[1] = KERN_CP_TIME;
        mib[2] = i;
        size = sizeof(cpu_time);
        if (sysctl(mib, 3, &cpu_time, &size, NULL, 0) == -1) {
            warn("failed to get kern.cptime2");
            PyErr_SetFromErrno(PyExc_OSError);
            return NULL;
        }

        py_cputime = Py_BuildValue(
            "(ddddd)",
            (double)cpu_time[CP_USER] / CLOCKS_PER_SEC,
            (double)cpu_time[CP_NICE] / CLOCKS_PER_SEC,
            (double)cpu_time[CP_SYS] / CLOCKS_PER_SEC,
            (double)cpu_time[CP_IDLE] / CLOCKS_PER_SEC,
            (double)cpu_time[CP_INTR] / CLOCKS_PER_SEC);
        if (!py_cputime)
            goto error;
        if (PyList_Append(py_retlist, py_cputime))
            goto error;
        Py_DECREF(py_cputime);
    }

    return py_retlist;

error:
    Py_XDECREF(py_cputime);
    Py_DECREF(py_retlist);
    return NULL;
}


PyObject *
psutil_disk_io_counters(PyObject *self, PyObject *args) {
    int i, dk_ndrive, mib[3];
    size_t len;
    struct io_sysctl *stats;

    PyObject *py_retdict = PyDict_New();
    PyObject *py_disk_info = NULL;
    if (py_retdict == NULL)
        return NULL;

    mib[0] = CTL_HW;
    mib[1] = HW_IOSTATS;
    mib[2] = sizeof(struct io_sysctl);
    len = 0;
    if (sysctl(mib, 3, NULL, &len, NULL, 0) < 0) {
        warn("can't get HW_IOSTATS");
        PyErr_SetFromErrno(PyExc_OSError);
        goto error;
    }
    dk_ndrive = (int)(len / sizeof(struct io_sysctl));

    stats = malloc(len);
    if (stats == NULL) {
        warn("can't malloc");
        PyErr_NoMemory();
        goto error;
    }
    if (sysctl(mib, 2, stats, &len, NULL, 0) < 0 ) {
        warn("could not read HW_IOSTATS");
        PyErr_SetFromErrno(PyExc_OSError);
        goto error;
    }

    for (i = 0; i < dk_ndrive; i++) {
        py_disk_info = Py_BuildValue(
            "(KKKKLL)",
            stats[i].rxfer,
            stats[i].wxfer,
            stats[i].rbytes,
            stats[i].wbytes,
            // assume half read - half writes.
            // TODO: why?
            (long long) PSUTIL_KPT2DOUBLE(stats[i].time) / 2,
            (long long) PSUTIL_KPT2DOUBLE(stats[i].time) / 2);
        if (!py_disk_info)
            goto error;
        if (PyDict_SetItemString(py_retdict, stats[i].name, py_disk_info))
            goto error;
        Py_DECREF(py_disk_info);
    }

    free(stats);
    return py_retdict;

error:
    Py_XDECREF(py_disk_info);
    Py_DECREF(py_retdict);
    if (stats != NULL)
        free(stats);
    return NULL;
}

