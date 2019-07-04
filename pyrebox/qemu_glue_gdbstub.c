/*-------------------------------------------------------------------------------

   Copyright (C) 2019 Cisco Talos Security Intelligence and Research Group

   PyREBox: Python scriptable Reverse Engineering Sandbox 
   Author: Xabier Ugarte-Pedrero 
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 2 as
   published by the Free Software Foundation.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA.
   
-------------------------------------------------------------------------------*/
#include <Python.h>
#include <limits.h>

//QEMU includes
#include "qemu/queue.h"
#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qstring.h"
#include "qemu/option.h"
#include "migration/vmstate.h"
#include "sysemu/sysemu.h"
#include "monitor/monitor.h"
#include "cpu.h"
#include "block/block.h"
#include "block/block_int.h"
#include "sysemu/block-backend.h"
#include "pyrebox/qemu_glue.h"
#include "pyrebox/qemu_glue_block.h"
#include "pyrebox/qemu_glue_gdbstub.h"
#include "pyrebox/utils.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"

#define MAX_PACKET_LENGTH 4096

#include "qemu/sockets.h"
#include "exec/semihost.h"
#include "exec/exec-all.h"

#include "pyrebox.h"

#define GDB_ATTACHED "1"

#define GDB_DEBUG_MODE 

#define TYPE_PYREBOX_CHARDEV_GDB "chardev-pyrebox-gdb"


static int sstep_flags = SSTEP_ENABLE|SSTEP_NOIRQ|SSTEP_NOTIMER;

typedef struct GDBRegisterState {
    int base_reg;
    int num_regs;
    gdb_reg_cb get_reg;
    gdb_reg_cb set_reg;
    const char *xml;
    struct GDBRegisterState *next;
} GDBRegisterState;


enum RSState {
    RS_INACTIVE,
    RS_IDLE,
    RS_GETLINE,
    RS_GETLINE_ESC,
    RS_GETLINE_RLE,
    RS_CHKSUM1,
    RS_CHKSUM2,
};

enum {
    GDB_SIGNAL_0 = 0,
    GDB_SIGNAL_INT = 2,
    GDB_SIGNAL_QUIT = 3,
    GDB_SIGNAL_TRAP = 5,
    GDB_SIGNAL_ABRT = 6,
    GDB_SIGNAL_ALRM = 14,
    GDB_SIGNAL_IO = 23,
    GDB_SIGNAL_XCPU = 24,
    GDB_SIGNAL_UNKNOWN = 143
};

typedef struct GDBState {
    /* Thread ID for subsequent operations */
    unsigned long long c_thread_id; /* current CPU for step/continue ops */
    unsigned long long g_thread_id; /* current CPU for other ops */
    unsigned int query_thread; /* for q{f|s}ThreadInfo */
    enum RSState state; /* parsing state */
    char line_buf[MAX_PACKET_LENGTH];
    int line_buf_index;
    int line_sum; /* running checksum */
    int line_csum; /* checksum at the end of the packet */
    uint8_t last_packet[MAX_PACKET_LENGTH + 4];
    int last_packet_len;
    CharBackend chr;
    Chardev *mon_chr;

    PyObject* current_threads;
    unsigned int number_of_current_threads;
    int vm_is_running;
} GDBState;

/* By default use no IRQs and no timers while single stepping so as to
 * make single stepping like an ICE HW step.
 */

static GDBState *pyrebox_gdbserver_state;

static bool pyrebox_gdb_has_xml;

static int gdb_num_regs = 0;

static unsigned long long int currently_running_thread = 0;

int pyrebox_put_packet_binary(GDBState *s, const char *buf, int len, bool dump);
int pyrebox_put_packet(GDBState *s, const char *buf);


//===========================  PRIMITIVES AND PYREBOX GLUE  ==========================================

static void pyrebox_update_threads(GDBState *s){
    //Update the list of running threads
    //Update the number of current threads
    if(s->current_threads){
        // Already up to date;
        return;
    }
    PyObject* py_module_name = PyString_FromString("vmi");
    PyObject* py_vmi_module = PyImport_Import(py_module_name);
    Py_DECREF(py_module_name);
    PyObject* py_get_threads = PyObject_GetAttrString(py_vmi_module,"get_threads");
    if (py_get_threads) {
        if (PyCallable_Check(py_get_threads)){
            PyObject* py_args = PyTuple_New(0);
            PyObject* ret = PyObject_CallObject(py_get_threads, py_args);
            Py_DECREF(py_args);
            if (ret) {
                s->current_threads = ret;
                s->number_of_current_threads = (unsigned int) PyObject_Size(ret);
                #ifdef GDB_DEBUG_MODE
                printf("Number of threads: %d\n", s->number_of_current_threads);
                #endif
            }
        }
    }
    PyErr_Print();
    return;
}

static int get_thread_description(GDBState* s, unsigned long long thread, char* buf, int len){
    // Calls python function to describe a thread, 
    // and returns the size of the description
    PyObject* py_module_name = PyString_FromString("vmi");
    PyObject* py_vmi_module = PyImport_Import(py_module_name);
    Py_DECREF(py_module_name);
    PyObject* py_get_thread_description = PyObject_GetAttrString(py_vmi_module,"get_thread_description");
    memset(buf, '\0', len);
    if (py_get_thread_description) {
        if (PyCallable_Check(py_get_thread_description)) {
            PyObject* py_args = PyTuple_New(2);
            PyTuple_SetItem(py_args, 0, PyLong_FromUnsignedLongLong(thread)); // The reference to the object in the tuple is stolen
            Py_INCREF(s->current_threads);
            PyTuple_SetItem(py_args, 1, s->current_threads); // The reference to the object in the tuple is stolen
            PyObject* ret = PyObject_CallObject(py_get_thread_description, py_args);
            Py_DECREF(py_args);
            if (ret) {
                // Copy the string
                const char* s = PyString_AsString(ret);
                strncpy(buf, s, len - 1);
                Py_DECREF(ret);
            }
        }
    }
    return strnlen(buf, len);
}

/* Return the GDB index for a given thread.
 *
 * The thread parameter just represents the numerical order in the thread
 * list, but the unsigned long long, is the actual thread indentifier. 
 */
static unsigned long long pyrebox_thread_gdb_index(GDBState* s, unsigned int thread)
{
    unsigned long long thread_id = 0;
    //Return the Thread ID.
    PyObject* py_module_name = PyString_FromString("vmi");
    PyObject* py_vmi_module = PyImport_Import(py_module_name);
    Py_DECREF(py_module_name);
    PyObject* py_get_thread_id = PyObject_GetAttrString(py_vmi_module,"get_thread_id");
    if (py_get_thread_id) {
        if (PyCallable_Check(py_get_thread_id)) {
            PyObject* py_args = PyTuple_New(2);
            PyTuple_SetItem(py_args, 0, PyLong_FromLong(thread)); // The reference to the object in the tuple is stolen
            Py_INCREF(s->current_threads);
            PyTuple_SetItem(py_args, 1, s->current_threads); // The reference to the object in the tuple is stolen
            PyObject* ret = PyObject_CallObject(py_get_thread_id, py_args);
            Py_DECREF(py_args);
            if (ret) {
                thread_id = PyLong_AsUnsignedLongLong(ret);
                Py_DECREF(ret);
            }
        }
    }
    return thread_id;
}

/* Get the thread id of the thread currently running on the first CPU */
static unsigned long long pyrebox_get_running_thread_first_cpu(GDBState* s){
    if (s->current_threads){
        unsigned long long thread_id = 0;
        //Return the Thread ID.
        PyObject* py_module_name = PyString_FromString("vmi");
        PyObject* py_vmi_module = PyImport_Import(py_module_name);
        Py_DECREF(py_module_name);
        PyObject* py_get_thread_id = PyObject_GetAttrString(py_vmi_module,"get_running_thread_first_cpu");
        if (py_get_thread_id) {
            if (PyCallable_Check(py_get_thread_id)) {
                PyObject* py_args = PyTuple_New(1);
                Py_INCREF(s->current_threads);
                PyTuple_SetItem(py_args, 0, s->current_threads); // The reference to the object in the tuple is stolen
                PyObject* ret = PyObject_CallObject(py_get_thread_id, py_args);
                Py_DECREF(py_args);
                if (ret) {
                    thread_id = PyLong_AsUnsignedLongLong(ret);
                    Py_DECREF(ret);
                }
            }
        }
        return thread_id;
    } else {
        return 0;
    }
}

// Check if a thread exists
static int does_thread_exist(GDBState* s, unsigned long long thread){
    // Calls python function to check if a thread exists 
    // and returns 0 if not, 1 if it exists
    PyObject* py_module_name = PyString_FromString("vmi");
    PyObject* py_vmi_module = PyImport_Import(py_module_name);
    Py_DECREF(py_module_name);
    PyObject* py_does_thread_exist = PyObject_GetAttrString(py_vmi_module,"does_thread_exist");
    if (py_does_thread_exist) {
        if (PyCallable_Check(py_does_thread_exist)) {
            PyObject* py_args = PyTuple_New(2);
            PyTuple_SetItem(py_args, 0, PyLong_FromUnsignedLongLong(thread)); // The reference to the object in the tuple is stolen
            Py_INCREF(s->current_threads);
            PyTuple_SetItem(py_args, 1, s->current_threads); // The reference to the object in the tuple is stolen
            PyObject* ret = PyObject_CallObject(py_does_thread_exist, py_args);
            Py_DECREF(py_args);
            if (ret) {
                int exists = PyObject_IsTrue(ret);
                Py_DECREF(ret);
                if (exists != -1){
                    return (exists ? 1: 0);
                }
            }
        }
    }
    return 0;
}

// Check if a thread exists
static int gdb_read_thread_register(GDBState* s, unsigned long long thread, int gdb_register_index, uint8_t* buf){
    // Calls python function to check if a thread exists 
    // and returns 0 if not, 1 if it exists
    PyObject* py_module_name = PyString_FromString("vmi");
    PyObject* py_vmi_module = PyImport_Import(py_module_name);
    Py_DECREF(py_module_name);
    PyObject* py_gdb_read_thread_register = PyObject_GetAttrString(py_vmi_module,"gdb_read_thread_register");
    if (py_gdb_read_thread_register) {
        if (PyCallable_Check(py_gdb_read_thread_register)) {
            PyObject* py_args = PyTuple_New(3);
            PyTuple_SetItem(py_args, 0, PyLong_FromUnsignedLongLong(thread)); // The reference to the object in the tuple is stolen
            Py_INCREF(s->current_threads);
            PyTuple_SetItem(py_args, 1, s->current_threads); // The reference to the object in the tuple is stolen
            //Add the gdb_register index
            PyTuple_SetItem(py_args, 2, PyLong_FromUnsignedLongLong(gdb_register_index)); // The reference to the object in the tuple is stolen
            PyObject* ret = PyObject_CallObject(py_gdb_read_thread_register, py_args);
            Py_DECREF(py_args);
            if (ret) {
                // Create a string from the returned value 
                char* tmp_str;
                Py_ssize_t length = 0;
                PyString_AsStringAndSize(ret, (char**) &tmp_str, &length);
                memcpy(buf, tmp_str, length);
                Py_DECREF(ret);
                return length;
            }
            PyErr_Print();
            return 0;
        }
        PyErr_Print();
        return 0;
    }
    PyErr_Print();
    return 0;
}


static int get_number_of_threads(GDBState* s){
    return s->number_of_current_threads;
}

static void pyrebox_vm_stop(GDBState* s){
    #ifdef GDB_DEBUG_MODE
    printf("Stopping VM...\n");
    #endif
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    }
    s->vm_is_running = 0;
    pyrebox_update_threads(s);
}

static void pyrebox_vm_start(GDBState* s){
    #ifdef GDB_DEBUG_MODE
    printf("Starting VM...\n");
    #endif
    vm_start();
    s->vm_is_running = 1;
    // Free the python object with the list of threads 
    if (s->current_threads){
        Py_DECREF(s->current_threads);
        s->current_threads = 0;
        s->number_of_current_threads = 0;
    }
}

static inline int target_memory_rw_debug(GDBState* s, unsigned long long thread, target_ulong addr,
                                         uint8_t *buf, int len, bool is_write)
{
    // Calls python function to check if a thread exists 
    // and returns 0 if not, 1 if it exists
    PyObject* py_module_name = PyString_FromString("vmi");
    PyObject* py_vmi_module = PyImport_Import(py_module_name);
    Py_DECREF(py_module_name);
    PyObject* py_gdb_memory_rw_debug = PyObject_GetAttrString(py_vmi_module, "gdb_memory_rw_debug");
    if (py_gdb_memory_rw_debug) {
        if (PyCallable_Check(py_gdb_memory_rw_debug)) {
            PyObject* py_args = PyTuple_New(6);
            PyTuple_SetItem(py_args, 0, PyLong_FromUnsignedLongLong(thread)); // The reference to the object in the tuple is stolen
            Py_INCREF(s->current_threads);
            PyTuple_SetItem(py_args, 1, s->current_threads); // The reference to the object in the tuple is stolen
            //Add the address and length, and is_write 
            #if TARGET_LONG_SIZE == 4
            PyTuple_SetItem(py_args, 2, PyLong_FromUnsignedLong(addr)); // The reference to the object in the tuple is stolen
            #elif TARGET_LONG_SIZE == 8
            PyTuple_SetItem(py_args, 2, PyLong_FromUnsignedLongLong(addr)); // The reference to the object in the tuple is stolen
            #else
            #error TARGET_LONG_SIZE undefined
            #endif
            PyTuple_SetItem(py_args, 3, PyLong_FromLong(len)); // The reference to the object in the tuple is stolen
            if (is_write){
                PyTuple_SetItem(py_args, 4, PyString_FromStringAndSize((const char*)buf, (Py_ssize_t) len)); // The reference to the object in the tuple is stolen
            } else {
                Py_INCREF(Py_None);
                PyTuple_SetItem(py_args, 4, Py_None); // The reference to the object in the tuple is stolen
            }
            PyTuple_SetItem(py_args, 5, PyBool_FromLong(is_write)); // The reference to the object in the tuple is stolen

            PyObject* ret = PyObject_CallObject(py_gdb_memory_rw_debug, py_args);
            Py_DECREF(py_args);
            if (ret) {
                // Create a string from the returned value 
                char* tmp_str;
                Py_ssize_t length = 0;
                PyString_AsStringAndSize(ret, (char**) &tmp_str, &length);
                PyErr_Print();
                if (!is_write){
                    memcpy(buf, tmp_str, length < len? length : len);
                }
                Py_DECREF(ret);
                return length;
            }
            PyErr_Print();
            return 0;
        }
        PyErr_Print();
        return 0;
    }
    PyErr_Print();
    return 0;
}

static void gdb_set_cpu_pc(GDBState *s, target_ulong pc)
{
    // Calls python function to set cpu PC 
    PyObject* py_module_name = PyString_FromString("vmi");
    PyObject* py_vmi_module = PyImport_Import(py_module_name);
    Py_DECREF(py_module_name);
    PyObject* py_gdb_set_cpu_pc = PyObject_GetAttrString(py_vmi_module,"gdb_set_cpu_pc");
    if (py_gdb_set_cpu_pc) {
        if (PyCallable_Check(py_gdb_set_cpu_pc)) {
            PyObject* py_args = PyTuple_New(3);
            PyTuple_SetItem(py_args, 0, PyLong_FromUnsignedLongLong(s->c_thread_id)); // The reference to the object in the tuple is stolen
            Py_INCREF(s->current_threads);
            PyTuple_SetItem(py_args, 1, s->current_threads); // The reference to the object in the tuple is stolen
            //Add the pc
            #if TARGET_LONG_SIZE == 4
            PyTuple_SetItem(py_args, 2, PyLong_FromUnsignedLong(pc)); // The reference to the object in the tuple is stolen
            #elif TARGET_LONG_SIZE == 8
            PyTuple_SetItem(py_args, 2, PyLong_FromUnsignedLongLong(pc)); // The reference to the object in the tuple is stolen
            #else
            #error TARGET_LONG_SIZE undefined
            #endif
            PyObject* ret = PyObject_CallObject(py_gdb_set_cpu_pc, py_args);
            Py_DECREF(py_args);
            if (ret) {
                // Create a string from the returned value 
                //int length = PyLong_AsLong(ret);
                Py_DECREF(ret);
                return;
            }
            PyErr_Print();
            return;
        }
        PyErr_Print();
        return;
    }
    PyErr_Print();
    return;
}

/* Resume execution.  */
static inline void pyrebox_gdb_continue(GDBState *s)
{
    if (!runstate_needs_reset()) {
        pyrebox_vm_start(s);
    }
}

static void pyrebox_gdb_breakpoint_remove_all(void)
{
    //XXX PyREBox primitive
    return;
}

static void pyrebox_cpu_single_step(unsigned long long thread, int activate){
    //XXX Pyrebox primitive
}

static int pyrebox_gdb_breakpoint_insert(target_ulong addr, target_ulong len, int type)
{
    //XXX Pyrebox primitive
    //CPUState *cpu;
    //int err = 0;

    switch (type) {
    case GDB_BREAKPOINT_SW:
    case GDB_BREAKPOINT_HW:
        /*CPU_FOREACH(cpu) {
            err = cpu_breakpoint_insert(cpu, addr, BP_GDB, NULL);
            if (err) {
                break;
            }
        }
        return err;*/
    case GDB_WATCHPOINT_WRITE:
    case GDB_WATCHPOINT_READ:
    case GDB_WATCHPOINT_ACCESS:
        /*
        CPU_FOREACH(cpu) {
            err = cpu_watchpoint_insert(cpu, addr, len,
                                        xlat_gdb_type(cpu, type), NULL);
            if (err) {
                break;
            }
        }
        return err; */
        return -ENOSYS;
    default:
        return -ENOSYS;
    }
}

static int pyrebox_gdb_breakpoint_remove(target_ulong addr, target_ulong len, int type)
{
    //XXX Pyrebox primitive
    //CPUState *cpu;
    //int err = 0;

    switch (type) {
    case GDB_BREAKPOINT_SW:
    case GDB_BREAKPOINT_HW:
        /*CPU_FOREACH(cpu) {
            err = cpu_breakpoint_remove(cpu, addr, BP_GDB);
            if (err) {
                break;
            }
        }
        return err;*/
    case GDB_WATCHPOINT_WRITE:
    case GDB_WATCHPOINT_READ:
    case GDB_WATCHPOINT_ACCESS:
        /*
        CPU_FOREACH(cpu) {
            err = cpu_watchpoint_remove(cpu, addr, len,
                                        xlat_gdb_type(cpu, type));
            if (err)
                break;
        }
        return err;
        */
    default:
        return -ENOSYS;
    }
}

void gdb_signal_breakpoint(unsigned int thread_index) {
    GDBState *s = pyrebox_gdbserver_state;
    pyrebox_update_threads(s);
    unsigned long long thread = pyrebox_thread_gdb_index(s, thread_index);
    char buf[256];

    //if (cpu->watchpoint_hit) {
    //    switch (cpu->watchpoint_hit->flags & BP_MEM_ACCESS) {
    //    case BP_MEM_READ:
    //        type = "r";
    //        break;
    //    case BP_MEM_ACCESS:
    //        type = "a";
    //        break;
    //    default:
    //        type = "";
    //        break;
    //    }
    //    snprintf(buf, sizeof(buf),
    //             "T%02xthread:%02x;%swatch:" TARGET_FMT_lx ";",
    //             GDB_SIGNAL_TRAP, cpu_gdb_index(cpu), type,
    //             (target_ulong)cpu->watchpoint_hit->vaddr);
    //    cpu->watchpoint_hit = NULL;
    //    put_packet(s, buf);
    //}

    // Flush all cpus:
    CPUState *cpu;
    for (cpu = first_cpu; cpu != NULL; cpu = CPU_NEXT(cpu)) {
        tb_flush(cpu);
    }
    s->c_thread_id = thread;
    s->g_thread_id = thread;
    snprintf(buf, sizeof(buf), "T%02xthread:%02llx;", GDB_SIGNAL_TRAP, thread);
    pyrebox_put_packet(s, buf);

    // disable single step if it was enabled 
    pyrebox_cpu_single_step(thread, 0);

    // Stop the CPU, so that it doesn't keep executing when we return
    pyrebox_vm_stop(s);
}

static int gdb_get_register_size(int gdb_register_index){
    PyObject* py_module_name = PyString_FromString("vmi");
    PyObject* py_vmi_module = PyImport_Import(py_module_name);
    Py_DECREF(py_module_name);
    PyObject* py_gdb_get_register_size = PyObject_GetAttrString(py_vmi_module,"gdb_get_register_size");
    if (py_gdb_get_register_size) {
        if (PyCallable_Check(py_gdb_get_register_size)) {
            PyObject* py_args = PyTuple_New(1);
            PyTuple_SetItem(py_args, 0, PyLong_FromUnsignedLongLong(gdb_register_index)); // The reference to the object in the tuple is stolen

            PyObject* ret = PyObject_CallObject(py_gdb_get_register_size, py_args);
            Py_DECREF(py_args);

            // Returns size written
            if (ret) {
                int ret_val = PyLong_AsLong(ret);
                Py_DECREF(ret);
                return ret_val;
            }
            PyErr_Print();
            return 0;
        }
        PyErr_Print();
        return 0;
    }
    PyErr_Print();
    return 0;

    
}

static int gdb_write_thread_register(GDBState* s, unsigned long long thread, int gdb_register_index, uint8_t* buf)
{
    int len = gdb_get_register_size(gdb_register_index);
    if (len <= 0){
        return 0;
    }
    // Calls python function to check if a thread exists 
    // and returns 0 if not, 1 if it exists
    PyObject* py_module_name = PyString_FromString("vmi");
    PyObject* py_vmi_module = PyImport_Import(py_module_name);
    Py_DECREF(py_module_name);
    PyObject* py_gdb_write_thread_register = PyObject_GetAttrString(py_vmi_module,"gdb_write_thread_register");
    if (py_gdb_write_thread_register) {
        if (PyCallable_Check(py_gdb_write_thread_register)) {
            PyObject* py_args = PyTuple_New(4);
            PyTuple_SetItem(py_args, 0, PyLong_FromUnsignedLongLong(thread)); // The reference to the object in the tuple is stolen
            Py_INCREF(s->current_threads);
            PyTuple_SetItem(py_args, 1, s->current_threads); // The reference to the object in the tuple is stolen
            //Add the gdb_register index
            PyTuple_SetItem(py_args, 2, PyLong_FromUnsignedLongLong(gdb_register_index)); // The reference to the object in the tuple is stolen
            //Add the buffer
            PyTuple_SetItem(py_args, 3, PyString_FromStringAndSize((const char*)buf, (Py_ssize_t) len)); // The reference to the object in the tuple is stolen

            PyObject* ret = PyObject_CallObject(py_gdb_write_thread_register, py_args);
            Py_DECREF(py_args);

            // Returns size written
            if (ret) {
                int ret_val = PyLong_AsLong(ret);
                Py_DECREF(ret);
                return ret_val;
            }
            PyErr_Print();
            return 0;
        }
        PyErr_Print();
        return 0;
    }
    PyErr_Print();
    return 0;
}


//===========================  HELPERS TO WRITE TO GDB SOCKET  ==========================================

static void pyrebox_put_buffer(GDBState *s, const uint8_t *buf, int len)
{
    /* XXX this blocks entire thread. Rewrite to use
     * qemu_chr_fe_write and background I/O callbacks */
    qemu_chr_fe_write_all(&s->chr, buf, len);
}

static inline int pyrebox_fromhex(int v)
{
    if (v >= '0' && v <= '9')
        return v - '0';
    else if (v >= 'A' && v <= 'F')
        return v - 'A' + 10;
    else if (v >= 'a' && v <= 'f')
        return v - 'a' + 10;
    else
        return 0;
}

static inline int pyrebox_tohex(int v)
{
    if (v < 10)
        return v + '0';
    else
        return v - 10 + 'a';
}

/* writes 2*len+1 bytes in buf */
static void pyrebox_memtohex(char *buf, const uint8_t *mem, int len)
{
    int i, c;
    char *q;
    q = buf;
    for(i = 0; i < len; i++) {
        c = mem[i];
        *q++ = pyrebox_tohex(c >> 4);
        *q++ = pyrebox_tohex(c & 0xf);
    }
    *q = '\0';
}

static void pyrebox_hextomem(uint8_t *mem, const char *buf, int len)
{
    int i;

    for(i = 0; i < len; i++) {
        mem[i] = (pyrebox_fromhex(buf[0]) << 4) | pyrebox_fromhex(buf[1]);
        buf += 2;
    }
}

/* return -1 if error, 0 if OK */
int pyrebox_put_packet_binary(GDBState *s, const char *buf, int len, bool dump)
{
    int csum, i;
    uint8_t *p;
    #ifdef GDB_DEBUG_MODE
    if(len > 0){
        printf("\033[0;31m");
        printf("%.*s\n", len, buf);
        printf("\033[0m");
        fflush(stdout);
    }
    #endif
    for(;;) {
        p = s->last_packet;
        *(p++) = '$';
        memcpy(p, buf, len);
        p += len;
        csum = 0;
        for(i = 0; i < len; i++) {
            csum += buf[i];
        }
        *(p++) = '#';
        *(p++) = pyrebox_tohex((csum >> 4) & 0xf);
        *(p++) = pyrebox_tohex((csum) & 0xf);

        s->last_packet_len = p - s->last_packet;
        pyrebox_put_buffer(s, (uint8_t *)s->last_packet, s->last_packet_len);

        break;
    }
    return 0;
}

/* return -1 if error, 0 if OK */
int pyrebox_put_packet(GDBState *s, const char *buf)
{
    return pyrebox_put_packet_binary(s, buf, strlen(buf), false);
}

/* Encode data using the encoding for 'x' packets.  */
static int pyrebox_memtox(char *buf, const char *mem, int len)
{
    char *p = buf;
    char c;

    while (len--) {
        c = *(mem++);
        switch (c) {
        case '#': case '$': case '*': case '}':
            *(p++) = '}';
            *(p++) = c ^ 0x20;
            break;
        default:
            *(p++) = c;
            break;
        }
    }
    return p - buf;
}


static const char *pyrebox_get_feature_xml(const char *p, const char **newp,
                                   CPUClass *cc)
{
    size_t len;
    int i;
    const char *name;
    static char target_xml[1024];

    len = 0;
    while (p[len] && p[len] != ':')
        len++;
    *newp = p + len;

    name = NULL;
    if (strncmp(p, "target.xml", len) == 0) {
        /* Generate the XML description for this CPU.  */
        if (!target_xml[0]) {
            GDBRegisterState *r;
            CPUState *cpu = first_cpu;

            pstrcat(target_xml, sizeof(target_xml),
                    "<?xml version=\"1.0\"?>"
                    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
                    "<target>");
            if (cc->gdb_arch_name) {
                gchar *arch = cc->gdb_arch_name(cpu);
                pstrcat(target_xml, sizeof(target_xml), "<architecture>");
                pstrcat(target_xml, sizeof(target_xml), arch);
                pstrcat(target_xml, sizeof(target_xml), "</architecture>");
                g_free(arch);
            }
            pstrcat(target_xml, sizeof(target_xml), "<xi:include href=\"");
            pstrcat(target_xml, sizeof(target_xml), cc->gdb_core_xml_file);
            pstrcat(target_xml, sizeof(target_xml), "\"/>");
            for (r = cpu->gdb_regs; r; r = r->next) {
                pstrcat(target_xml, sizeof(target_xml), "<xi:include href=\"");
                pstrcat(target_xml, sizeof(target_xml), r->xml);
                pstrcat(target_xml, sizeof(target_xml), "\"/>");
            }
            pstrcat(target_xml, sizeof(target_xml), "</target>");
        }
        return target_xml;
    }
    for (i = 0; ; i++) {
        name = xml_builtin[i][0];
        if (!name || (strncmp(name, p, len) == 0 && strlen(name) == len))
            break;
    }
    return name ? xml_builtin[i][1] : NULL;
}

static void pyrebox_gdb_signal_trap(GDBState *s, unsigned long long thread){
    char buf[MAX_PACKET_LENGTH+ 1 /* trailing NUL */];
    currently_running_thread = thread;
    snprintf(buf, sizeof(buf), "T%02xthread:%llx;", GDB_SIGNAL_TRAP, thread);
    pyrebox_put_packet(s, buf);
}


//===========================  COMMANDS TO GDB  ==========================================


/* Tell the remote gdb that the process has exited.  */
// XXX: This event must be signaled when the debugged
// process exits.
void pyrebox_gdb_exit(CPUArchState *env, int code)
{
  GDBState *s;
  char buf[4];

  s = pyrebox_gdbserver_state;
  if (!s) {
      return;
  }

  snprintf(buf, sizeof(buf), "W%02x", (uint8_t)code);
  pyrebox_put_packet(s, buf);

  qemu_chr_fe_deinit(&s->chr, true);
}

//===========================  GDB PROTOCOL PARSER =================================================

static int is_query_packet(const char *p, const char *query, char separator)
{
    unsigned int query_len = strlen(query);

    return strncmp(p, query, query_len) == 0 &&
        (p[query_len] == '\0' || p[query_len] == separator);
}

// This function implements the actions for every packet
static int pyrebox_gdb_handle_packet(GDBState *s, const char *line_buf)
{
    CPUClass *cc;
    const char *p;
    uint32_t thread;
    int ch, reg_size, type, res;
    uint8_t mem_buf[MAX_PACKET_LENGTH];
    char buf[sizeof(mem_buf) + 1 /* trailing NUL */];
    uint8_t *registers;
    int addr, len;

    p = line_buf;
    ch = *p++;
    switch(ch) {
    case '?':
        // Returns the number for the TRAP signal, and the 
        // thread id / task id that is being executed.
        pyrebox_gdb_signal_trap(s, pyrebox_get_running_thread_first_cpu(s));

        /* Remove all the breakpoints when this query is issued,
         * because gdb is doing and initial connect and the state
         * should be cleaned up.
         */
        pyrebox_gdb_breakpoint_remove_all();
        break;
    case 'c':
        //Continue and (optionally) set address
        if (*p != '\0') {
            addr = strtoull(p, (char **)&p, 16);
            gdb_set_cpu_pc(s, addr);
        }
        pyrebox_gdb_continue(s);
        return RS_IDLE;
    case 'C':
        // Continue with signal
        pyrebox_gdb_continue(s);
        return RS_IDLE;
    case 'v':
        // Check if there is support for vCont
        // We always return false
        if (strncmp(p, "Cont", 4) == 0) {
            p += 4;
            if (*p == '?') {
                // We dont support the vCont packet, as it
                // allows to specify continue and step actions
                // for threads individually. As we will be
                // pausing the entire machine on breakpoints,
                // this doesn't apply.
                //pyrebox_put_packet(s, "vCont;c;C;s;S");
                //break;
                
                // An empty packet means we don't support it.
                /* put empty packet */
                pyrebox_put_packet(s, "");
                break;
            } else {
                    goto unknown_command;
                }
            } else {
                goto unknown_command;
            }
    case 'k':
        /* Kill the target */
        error_report("QEMU: Terminated via GDBstub");
        exit(0);
    case 'D':
        // Detach packet
        /* Detach packet */
        pyrebox_gdb_breakpoint_remove_all();
        pyrebox_gdb_continue(s);
        pyrebox_put_packet(s, "OK");
        break;
    case 's':
        // Single step
        if (*p != '\0') {
            addr = strtoull(p, (char **)&p, 16);
            gdb_set_cpu_pc(s, addr);
        }
        pyrebox_cpu_single_step(s->c_thread_id, 1);
        pyrebox_gdb_continue(s);
        return RS_IDLE;
    // case 'F':
    // This corresponds to the GDB File IO extension that allows the GDB target (debugee)
    // to interact with the debugger's file system, and to execute certain system
    // calls on the debugger. We just do not implement this.
    case 'g':
        // Read registers
        #ifdef GDB_DEBUG_MODE
        printf("Reading registers for command g... for thread: %llx\n", s->g_thread_id);
        #endif
        len = 0;
        for (addr = 0; addr < gdb_num_regs; addr++) {
            reg_size = gdb_read_thread_register(s, s->g_thread_id, addr, mem_buf + len);
            #ifdef GDB_DEBUG_MODE
            printf("Reading register for command g...\n");
            #endif
            len += reg_size;
        }
        #ifdef GDB_DEBUG_MODE
        printf("Finished reading register for command g...\n");
        #endif
        pyrebox_memtohex(buf, mem_buf, len);
        #ifdef GDB_DEBUG_MODE
        printf("Sending buffer (%d): %s\n", len, buf);
        #endif
        pyrebox_put_packet(s, buf);
        break;
    case 'G':
        // Write registers
        #ifdef GDB_DEBUG_MODE
        printf("Writing registers for command g... for thread: %llx\n", s->g_thread_id);
        #endif
        registers = mem_buf;
        len = strlen(p) / 2;
        pyrebox_hextomem((uint8_t *)registers, p, len);
        for (addr = 0; addr < gdb_num_regs && len > 0; addr++) {
            reg_size = gdb_write_thread_register(s, s->g_thread_id, addr, registers);
            #ifdef GDB_DEBUG_MODE
            printf("Writing register for command g...\n");
            #endif
            len -= reg_size;
            registers += reg_size;
        }
        #ifdef GDB_DEBUG_MODE
        printf("Finished writing register for command g...\n");
        #endif
        pyrebox_put_packet(s, "OK");
        break;
    case 'm':
        // Read memory
        addr = strtoull(p, (char **)&p, 16);
        if (*p == ',')
            p++;
        len = strtoull(p, NULL, 16);

        /* pyrebox_memtohex() doubles the required space */
        if (len > MAX_PACKET_LENGTH / 2) {
            pyrebox_put_packet (s, "E22");
            break;
        }

        if (!target_memory_rw_debug(s, s->g_thread_id, addr, mem_buf, len, false) != 0) {
            pyrebox_put_packet (s, "E14");
        } else {
            pyrebox_memtohex(buf, mem_buf, len);
            pyrebox_put_packet(s, buf);
        }
        break;
    case 'M':
        // Write memory
        addr = strtoull(p, (char **)&p, 16);
        if (*p == ',')
            p++;
        len = strtoull(p, (char **)&p, 16);
        if (*p == ':')
            p++;

        /* pyrebox_hextomem() reads 2*len bytes */
        if (len > strlen(p) / 2) {
            pyrebox_put_packet (s, "E22");
            break;
        }
        pyrebox_hextomem(mem_buf, p, len);
        if (!target_memory_rw_debug(s, s->g_thread_id, addr, mem_buf, len,
                                   true) != 0) {
            pyrebox_put_packet(s, "E14");
        } else {
            pyrebox_put_packet(s, "OK");
        }
        break;
    case 'p':
        // Read regiser, old format
        /* Older gdb are really dumb, and don't use 'g' if 'p' is available.
           This works, but can be very slow.  Anything new enough to
           understand XML also knows how to use this properly.  */
        if (!pyrebox_gdb_has_xml)
            goto unknown_command;
        addr = strtoull(p, (char **)&p, 16);
        reg_size = gdb_read_thread_register(s, s->g_thread_id, addr, mem_buf);
        if (reg_size) {
            pyrebox_memtohex(buf, mem_buf, reg_size);
            pyrebox_put_packet(s, buf);
        } else {
            pyrebox_put_packet(s, "E14");
        }
        break;
    case 'P':
        // Write register, old format
        if (!pyrebox_gdb_has_xml)
            goto unknown_command;
        addr = strtoull(p, (char **)&p, 16);
        if (*p == '=')
            p++;
        reg_size = strlen(p) / 2;
        pyrebox_hextomem(mem_buf, p, reg_size);
        gdb_write_thread_register(s, s->g_thread_id, addr, mem_buf);
        pyrebox_put_packet(s, "OK");
        break;
    case 'Z':
    case 'z':
        // Insert breakpoint
        type = strtoul(p, (char **)&p, 16);
        if (*p == ',')
            p++;
        addr = strtoull(p, (char **)&p, 16);
        if (*p == ',')
            p++;
        len = strtoull(p, (char **)&p, 16);
        if (ch == 'Z')
            res = pyrebox_gdb_breakpoint_insert(addr, len, type);
        else
            res = pyrebox_gdb_breakpoint_remove(addr, len, type);
        if (res >= 0)
             pyrebox_put_packet(s, "OK");
        else if (res == -ENOSYS)
            pyrebox_put_packet(s, "");
        else
            pyrebox_put_packet(s, "E22");
        break;
    case 'H':
        /* Set thread for subsequent operations */
        type = *p++;
        thread = strtoull(p, (char **)&p, 16);
        if (thread == -1){
            thread = 0;  
        } else if (thread == 0) {
            thread = currently_running_thread; 
        }
        switch (type) {
        case 'c':
            s->c_thread_id = thread;
            pyrebox_put_packet(s, "OK");
            break;
        case 'g':
            s->g_thread_id = thread;
            pyrebox_put_packet(s, "OK");
            break;
        default:
             pyrebox_put_packet(s, "E22");
             break;
        }
        break;
    case 'T':
        // Find out if thread X is alive
        thread = strtoull(p, (char **)&p, 16);
        if (does_thread_exist(s, thread)){
            pyrebox_put_packet(s, "OK");
        } else {
            pyrebox_put_packet(s, "E22");
        }
        break;
    case 'q':
    case 'Q':
        /* parse any 'q' packets here */
        if (!strcmp(p,"qemu.sstepbits")) {
            /* Query Breakpoint bit definitions */
            snprintf(buf, sizeof(buf), "ENABLE=%x,NOIRQ=%x,NOTIMER=%x",
                     SSTEP_ENABLE,
                     SSTEP_NOIRQ,
                     SSTEP_NOTIMER);
            pyrebox_put_packet(s, buf);
            break;
        } else if (is_query_packet(p, "qemu.sstep", '=')) {
            /* Display or change the sstep_flags */
            p += 10;
            if (*p != '=') {
                /* Display current setting */
                snprintf(buf, sizeof(buf), "0x%x", sstep_flags);
                pyrebox_put_packet(s, buf);
                break;
            }
            p++;
            type = strtoul(p, (char **)&p, 16);
            sstep_flags = type;
            pyrebox_put_packet(s, "OK");
            break;
        } else if (strcmp(p,"C") == 0) {
            /* "Current thread" remains vague in the spec, so always return
             *  the first CPU (gdb returns the first thread). */
            pyrebox_put_packet(s, "QC1");
            break;
        } else if (strcmp(p,"fThreadInfo") == 0) {
            s->query_thread = 0;
            goto report_cpuinfo;
        } else if (strcmp(p,"sThreadInfo") == 0) {
        report_cpuinfo:
            if (s->query_thread < get_number_of_threads(s)) {
                snprintf(buf, sizeof(buf), "m%llx", pyrebox_thread_gdb_index(s, s->query_thread));
                pyrebox_put_packet(s, buf);
                s->query_thread = s->query_thread + 1;
            } else {
                pyrebox_put_packet(s, "l");
            }
            break;
        } else if (strncmp(p, "ThreadExtraInfo,", 16) == 0) {
            thread = strtoull(p + 16, (char **)&p, 16);
            len = get_thread_description(s, thread, (char*) mem_buf, sizeof(buf) / 2);
            pyrebox_memtohex(buf, mem_buf, len);
            pyrebox_put_packet(s, buf);
            break;
        }
        // Run QEMU command
        else if (strncmp(p, "Rcmd,", 5) == 0) {
            // Run QEMU command
            int len = strlen(p + 5);

            if ((len % 2) != 0) {
                pyrebox_put_packet(s, "E01");
                break;
            }
            len = len / 2;
            pyrebox_hextomem(mem_buf, p + 5, len);
            mem_buf[len++] = 0;
            qemu_chr_be_write(s->mon_chr, mem_buf, len);
            pyrebox_put_packet(s, "OK");
            break;
        }
        if (is_query_packet(p, "Supported", ':')) {
            snprintf(buf, sizeof(buf), "PacketSize=%x", MAX_PACKET_LENGTH);
            cc = CPU_GET_CLASS(first_cpu);
            if (cc->gdb_core_xml_file != NULL) {
                pstrcat(buf, sizeof(buf), ";qXfer:features:read+");
            }
            pyrebox_put_packet(s, buf);
            break;
        }
        if (strncmp(p, "Xfer:features:read:", 19) == 0) {
            const char *xml;
            target_ulong total_len;

            cc = CPU_GET_CLASS(first_cpu);
            if (cc->gdb_core_xml_file == NULL) {
                goto unknown_command;
            }

            pyrebox_gdb_has_xml = true;
            p += 19;
            xml = pyrebox_get_feature_xml(p, &p, cc);
            if (!xml) {
                snprintf(buf, sizeof(buf), "E00");
                pyrebox_put_packet(s, buf);
                break;
            }

            if (*p == ':')
                p++;
            addr = strtoul(p, (char **)&p, 16);
            if (*p == ',')
                p++;
            len = strtoul(p, (char **)&p, 16);

            total_len = strlen(xml);
            if (addr > total_len) {
                snprintf(buf, sizeof(buf), "E00");
                pyrebox_put_packet(s, buf);
                break;
            }
            if (len > (MAX_PACKET_LENGTH - 5) / 2)
                len = (MAX_PACKET_LENGTH - 5) / 2;
            if (len < total_len - addr) {
                buf[0] = 'm';
                len = pyrebox_memtox(buf + 1, xml + addr, len);
            } else {
                buf[0] = 'l';
                len = pyrebox_memtox(buf + 1, xml + addr, total_len - addr);
            }
            pyrebox_put_packet_binary(s, buf, len + 1, true);
            break;
        }
        if (is_query_packet(p, "Attached", ':')) {
            pyrebox_put_packet(s, GDB_ATTACHED);
            break;
        }
        /* Unrecognised 'q' command.  */
        goto unknown_command;

    default:
    unknown_command:
        /* put empty packet */
        buf[0] = '\0';
        pyrebox_put_packet(s, buf);
        break;
    }
    return RS_IDLE;
}


// This function handles single characters, and calls handle_packet for each command
static void pyrebox_gdb_read_byte(GDBState *s, int ch)
{
    #ifdef GDB_DEBUG_MODE
    printf("\033[0;32m");
    printf("%c", (char) ch);
    printf("\033[0m");
    fflush(stdout);
    #endif

    uint8_t reply;

    if (s->last_packet_len) {
        /* Waiting for a response to the last packet.  If we see the start
           of a new command then abandon the previous response.  */

        // - is retrasmission required, so we do the retransmission
        if (ch == '-') {
            pyrebox_put_buffer(s, (uint8_t *)s->last_packet, s->last_packet_len);
        }
        // + means ACK. Packets are enclosed between $ and #
        if (ch == '+' || ch == '$')
            s->last_packet_len = 0;
        if (ch != '$')
            return;
    }
    if (runstate_is_running()) {
        /* when the CPU is running, we cannot do anything except stopping
           it when receiving a char */
        pyrebox_vm_stop(s);
        // The cpu was running and we got an interrupt, so we 
        // just get the thread running on the first CPU.
        pyrebox_gdb_signal_trap(s, pyrebox_get_running_thread_first_cpu(s));
    } else {
        switch(s->state) {
        case RS_IDLE:
            if (ch == '$') {
                /* start of command packet */
                s->line_buf_index = 0;
                s->line_sum = 0;
                s->state = RS_GETLINE;
            }            break;
        case RS_GETLINE:
            if (ch == '}') {
                /* start escape sequence */
                s->state = RS_GETLINE_ESC;
                s->line_sum += ch;
            } else if (ch == '*') {
                /* start run length encoding sequence */
                s->state = RS_GETLINE_RLE;
                s->line_sum += ch;
            } else if (ch == '#') {
                /* end of command, start of checksum*/
                s->state = RS_CHKSUM1;
            } else if (s->line_buf_index >= sizeof(s->line_buf) - 1) {
                s->state = RS_IDLE;
            } else {
                /* unescaped command character */
                s->line_buf[s->line_buf_index++] = ch;
                s->line_sum += ch;
            }
            break;
        case RS_GETLINE_ESC:
            if (ch == '#') {
                /* unexpected end of command in escape sequence */
                s->state = RS_CHKSUM1;
            } else if (s->line_buf_index >= sizeof(s->line_buf) - 1) {
                /* command buffer overrun */
                s->state = RS_IDLE;
            } else {
                /* parse escaped character and leave escape state */
                s->line_buf[s->line_buf_index++] = ch ^ 0x20;
                s->line_sum += ch;
                s->state = RS_GETLINE;
            }
            break;
        case RS_GETLINE_RLE:
            if (ch < ' ') {
                /* invalid RLE count encoding */
                s->state = RS_GETLINE;
            } else {
                /* decode repeat length */
                int repeat = (unsigned char)ch - ' ' + 3;
                if (s->line_buf_index + repeat >= sizeof(s->line_buf) - 1) {
                    /* that many repeats would overrun the command buffer */
                    s->state = RS_IDLE;
                } else if (s->line_buf_index < 1) {
                    /* got a repeat but we have nothing to repeat */
                    s->state = RS_GETLINE;
                } else {
                    /* repeat the last character */
                    memset(s->line_buf + s->line_buf_index,
                           s->line_buf[s->line_buf_index - 1], repeat);
                    s->line_buf_index += repeat;
                    s->line_sum += ch;
                    s->state = RS_GETLINE;
                }
            }
            break;
        case RS_CHKSUM1:
            /* get high hex digit of checksum */
            if (!isxdigit(ch)) {
                s->state = RS_GETLINE;
                break;
            }
            s->line_buf[s->line_buf_index] = '\0';
            s->line_csum = pyrebox_fromhex(ch) << 4;
            s->state = RS_CHKSUM2;
            break;
        case RS_CHKSUM2:
            /* get low hex digit of checksum */
            if (!isxdigit(ch)) {
                s->state = RS_GETLINE;
                break;
            }
            s->line_csum |= pyrebox_fromhex(ch);

            if (s->line_csum != (s->line_sum & 0xff)) {
                /* send NAK reply */
                reply = '-';
                pyrebox_put_buffer(s, &reply, 1);
                s->state = RS_IDLE;
            } else {
                /* send ACK reply */
                reply = '+';
                pyrebox_put_buffer(s, &reply, 1);
                s->state = pyrebox_gdb_handle_packet(s, s->line_buf);
            }
            break;
        default:
            abort();
        }
    }
}


//===========================  CALLBACKS GDB DATA RECEIVE ==========================================


static int pyrebox_gdb_chr_can_receive(void *opaque)
{
  /* We can handle an arbitrarily large amount of data.
   Pick the maximum packet size, which is as good as anything.  */
  return MAX_PACKET_LENGTH;
}

static void pyrebox_gdb_chr_receive(void *opaque, const uint8_t *buf, int size)
{
    //#ifdef GDB_DEBUG_MODE
    //printf("\033[0;33m");
    //printf("Receiving packet...\n\n");
    //printf("%.*s\n", size, buf);
    //printf("\n");
    //printf("\033[0m");
    //fflush(stdout);
    //#endif

    int i;
    for (i = 0; i < size; i++) {
        pyrebox_gdb_read_byte(pyrebox_gdbserver_state, buf[i]);
    }
}

static void pyrebox_gdb_chr_event(void *opaque, int event)
{
    GDBState* s = (GDBState*) opaque;
    switch (event) {
    case CHR_EVENT_OPENED:
        pyrebox_vm_stop(s);
        currently_running_thread = pyrebox_get_running_thread_first_cpu(s);
        #ifdef GDB_DEBUG_MODE
        printf("(1) Stopping VM...\n");
        #endif
        // This event happens when a new connection is established
        // on the GDB socket
        pyrebox_gdb_has_xml = false;
        break;
    default:
        break;
    }
}

//===========================  STARTUP AND CLEANUP ==========================================


int pyrebox_gdbserver_start(unsigned int port)
{
    GDBState *s = 0;
    char gdbstub_device_name[128];
    Chardev *chr = NULL;
    Chardev *mon_chr;

    if (!first_cpu) {
        utils_print_error("gdbstub: meaningless to attach gdb to a "
                     "machine without any CPU.");
        return -1;
    }

    // Avoid priviledged ports.
    if (port < 1024)
        return -1;
    
    snprintf(gdbstub_device_name, sizeof(gdbstub_device_name),
             "tcp::%u,nowait,nodelay,server", port);

    chr = qemu_chr_new_noreplay("pyrebox_gdb", gdbstub_device_name);
    if (!chr)
        return -1;

    s = pyrebox_gdbserver_state;
    if (!s) {
        s = g_malloc0(sizeof(GDBState));
        pyrebox_gdbserver_state = s;

        //XXX: This is used to signal breakpoints, this should be changed
        //XXX: In any case, we need to look at how breakpoints are signaled
        //     to do the same and signal the gdb client
        //qemu_add_vm_change_state_handler(pyrebox_gdb_vm_state_change, NULL);

        /* Initialize a monitor terminal for gdb */
        /* This monitor terminal allows to send QEMU commands from the gdb
         * client to the QEMU monitor. Would it be interesting to redirect
         * these commands to pyrebox ipython interface? */
        mon_chr = qemu_chardev_new(NULL, TYPE_PYREBOX_CHARDEV_GDB,
                                   NULL, &error_abort);
        monitor_init(mon_chr, 0);
    } else {
        qemu_chr_fe_deinit(&s->chr, true);
        mon_chr = s->mon_chr;
        memset(s, 0, sizeof(GDBState));
        s->mon_chr = mon_chr;
    }

    s->c_thread_id = 0;
    s->g_thread_id = 0;

    CPUClass *cc = CPU_GET_CLASS(first_cpu);
    gdb_num_regs = cc->gdb_num_core_regs;
    
    if (chr) {
        qemu_chr_fe_init(&s->chr, chr, &error_abort);
        qemu_chr_fe_set_handlers(&s->chr, pyrebox_gdb_chr_can_receive, pyrebox_gdb_chr_receive,
                                 pyrebox_gdb_chr_event, NULL, s, NULL, true);
    }
    s->state = chr ? RS_IDLE : RS_INACTIVE;
    s->mon_chr = mon_chr;

    // Initialize reference to python 
    // object for keeping the list of threads
    s->current_threads = 0;
    // Number of threads, kept to traverse the
    // list of threads
    s->number_of_current_threads = 0;
    // Status of the VM
    s->vm_is_running = 1;

    return 0;
}

void pyrebox_gdbserver_cleanup(void)
{
    if (pyrebox_gdbserver_state) {
        pyrebox_put_packet(pyrebox_gdbserver_state, "W00");
    }
}

//========================================= MONITOR ==========================================

/* Set up a QEMU monitor to allow routing commands from GDB
 * to QEMU monitor (both QEMU and PYREBOX commands) */

static void pyrebox_gdb_monitor_output(GDBState *s, const char *msg, int len)
{
    char buf[MAX_PACKET_LENGTH];

    buf[0] = 'O';
    if (len > (MAX_PACKET_LENGTH/2) - 1)
        len = (MAX_PACKET_LENGTH/2) - 1;
    pyrebox_memtohex(buf + 1, (uint8_t *)msg, len);

    // Outputs through the GDB socket in
    // GDB format
    pyrebox_put_packet(s, buf);
}

static int pyrebox_gdb_monitor_write(Chardev *chr, const uint8_t *buf, int len)
{
    const char *p = (const char *)buf;
    int max_sz;

    max_sz = (sizeof(pyrebox_gdbserver_state->last_packet) - 2) / 2;
    for (;;) {
        if (len <= max_sz) {
            pyrebox_gdb_monitor_output(pyrebox_gdbserver_state, p, len);
            break;
        }
        pyrebox_gdb_monitor_output(pyrebox_gdbserver_state, p, max_sz);
        p += max_sz;
        len -= max_sz;
    }
    return len;
}

static void pyrebox_gdb_monitor_open(Chardev *chr, ChardevBackend *backend,
                             bool *be_opened, Error **errp)
{
    *be_opened = false;
}

static void pyrebox_char_gdb_class_init(ObjectClass *oc, void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->internal = true;
    cc->open = pyrebox_gdb_monitor_open;
    cc->chr_write = pyrebox_gdb_monitor_write;
}


static const TypeInfo pyrebox_char_gdb_type_info = {
    .name = TYPE_PYREBOX_CHARDEV_GDB,
    .parent = TYPE_CHARDEV,
    .class_init = pyrebox_char_gdb_class_init,
};

static void pyrebox_register_types(void)
{
    type_register_static(&pyrebox_char_gdb_type_info);
}

type_init(pyrebox_register_types);