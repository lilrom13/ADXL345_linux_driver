/*
 * ADXL345 driver
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/wait.h>

// buffer definition

#define BUFFER_SIZE 64

typedef struct circular_buffer {
    void *buffer;     // data buffer
    void *buffer_end; // end of data buffer
    size_t capacity;  // maximum number of items in the buffer
    size_t count;     // number of items in the buffer
    size_t sz;        // size of each item in the buffer
    void *head;       // pointer to head
    void *tail;       // pointer to tail
} circular_buffer;

circular_buffer * cb_init(size_t capacity, size_t sz) {
    circular_buffer *cb = (circular_buffer *) kzalloc(sizeof(circular_buffer), GFP_KERNEL);

    if(cb == NULL)
        return NULL;

    cb->buffer = kzalloc(capacity * sz, GFP_KERNEL);

    if (cb->buffer == NULL)
      return NULL;

    cb->buffer_end = (char *)cb->buffer + capacity * sz;
    cb->capacity = capacity;
    cb->count = 0;
    cb->sz = sz;
    cb->head = cb->buffer;
    cb->tail = cb->buffer;

    return cb;
}

void cb_free(circular_buffer *cb) {
    if (cb) {
      if (cb->buffer)
        kfree(cb->buffer);
      kfree(cb);
    }
}

int cb_push_back(circular_buffer *cb, const void *item) {
    if(cb->count == cb->capacity){
      pr_err("Maximum buffer capacity\n");
      return 0;
    }
    memcpy(cb->head, item, cb->sz);
    cb->head = (char*)cb->head + cb->sz;
    if(cb->head == cb->buffer_end)
        cb->head = cb->buffer;
    cb->count++;

    return 1;
}

int cb_pop_front(circular_buffer *cb, void *item) {

    if(cb->count == 0){
      pr_err("Cannot remove from empty buffer");
      return -1;
    }
    memcpy(item, cb->tail, cb->sz);
    cb->tail = (char*)cb->tail + cb->sz;
    if(cb->tail == cb->buffer_end)
        cb->tail = cb->buffer;
    cb->count--;

    return 1;
}

// ----------------------------------------------------------------- //

#define DRIVER_AUTHOR "Romain, Margheriti, romain.margheriti@telecom-paristech.fr"
#define DRIVER_DESC   "Driver for ADXL345"

static inline char get_bits(char *x) {
    return ((x[0]>>2) & 0x3f) | x[1] << 6;
}

typedef enum AXIS_E { X = 1000, Y = 1001, Z = 1002 } AXIS;
// x,y,z
static const char AXIS_REGISTER[3] = { 0x32, 0x34, 0x36 };

typedef struct adxl345_device_t {
  char                ENABLE_AXIS;
  struct              miscdevice miscdev;
  circular_buffer    *buffer;
  wait_queue_head_t   queue;
} adxl345_device;

adxl345_device  *init_adxl345_device(struct i2c_client *client);
void             readAllTheFifo(struct i2c_client *client);
irqreturn_t      adxl345_irq_fn(int irq, void *arg);
static int       adi_adxl345_probe(struct i2c_client *client, const struct i2c_device_id *id);
static int       adi_adxl345_remove(struct i2c_client *client);
ssize_t          adi_adxl345_read(struct file *file, char __user *buf, size_t count, loff_t *f_pos);
long             adi_adxl345_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

static struct i2c_device_id adi_adxl345_idtable[] = {
  { "adi,adxl345", 0 }, {}
};
MODULE_DEVICE_TABLE(i2c, adi_adxl345_idtable);

static struct of_device_id adi_adxl345_of_match[] = {
  {
    .compatible = "adi,adxl345",
    .data = NULL
  }, {}
};
MODULE_DEVICE_TABLE(of, adi_adxl345_of_match);

static struct i2c_driver adi_adxl345_driver = {
    .driver = {
        /* Le champ name doit correspondre au nom du module
           et ne doit pas contenir d'espace */
        .name   = "adi_adxl345_driver",
        .of_match_table = of_match_ptr(adi_adxl345_of_match),
    },

    .id_table       = adi_adxl345_idtable,
    .probe          = adi_adxl345_probe,
    .remove         = adi_adxl345_remove,
};
module_i2c_driver(adi_adxl345_driver);

static const struct file_operations adi_adxl345_fops = {
    .owner          = THIS_MODULE,
    .read           = adi_adxl345_read,
    .unlocked_ioctl = adi_adxl345_unlocked_ioctl
};

ssize_t adi_adxl345_read(struct file *file, char __user *buf, size_t count, loff_t *f_pos)
{
  // struct miscdevice *miscdev  = (struct miscdevice *) file->private_data;
  // struct i2c_client *client   = to_i2c_client(miscdev->parent);
  // adxl345_device    *dev      = i2c_get_clientdata(client);
  // char   DATA[2]              = { 0, 0};
  //
  // i2c_master_send(client, &dev->ENABLE_AXIS, 1);
  // i2c_master_recv(client, DATA, 2);
  //
  // if (count == 1) {
  //   char a = ((DATA[0]>>2) & 0x3f) | DATA[1] << 6;
  //
  //   copy_to_user(buf,&a, 1);
  //
  //   return 1;
  // } else {
  //   copy_to_user(buf,&DATA, 2);
  //
  //   return 2;
  // }

  // test if the buffer is empty
  struct miscdevice *miscdev    = (struct miscdevice *) file->private_data;
  struct i2c_client *client     = to_i2c_client(miscdev->parent);
  adxl345_device    *dev        = i2c_get_clientdata(client);
  int                buff_size  = dev->buffer->count;

  // test if buffer is empty
  if (buff_size == 0)
    wait_event(dev->queue, ( (buff_size = dev->buffer->count) > 0));

  if (count > buff_size) {
    // user ask for more data then we have
    copy_to_user(buf,  dev->buffer->buffer, buff_size);

    return buff_size;
  } else {
    // user ask for less data then max we have
    copy_to_user(buf,  dev->buffer->buffer, count);

    return count;
  }
}

long adi_adxl345_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
  struct miscdevice *miscdev = (struct miscdevice *) file->private_data;
  struct i2c_client *client  = to_i2c_client(miscdev->parent);
  adxl345_device    *dev     = i2c_get_clientdata(client);

  switch (cmd) {
    case X:
      dev->ENABLE_AXIS = AXIS_REGISTER[0];
    break;
    case Y:
      dev->ENABLE_AXIS = AXIS_REGISTER[1];
    break;
    case Z:
      dev->ENABLE_AXIS = AXIS_REGISTER[2];
    break;
    default:
      dev->ENABLE_AXIS = AXIS_REGISTER[0];
    break;
  }
  return 0;
}

void readAllTheFifo(struct i2c_client *client) {
  int i = 0;
  char data[6];

  for (i = 0; i < 32; i++) {
    i2c_master_send(client, &AXIS_REGISTER[0], 1);
    i2c_master_recv(client, data, 6);
  }
}

irqreturn_t adxl345_irq_fn(int irq, void *arg) {
  struct i2c_client *client = (struct i2c_client *) arg;
  adxl345_device    *dev    = i2c_get_clientdata(client);
  char  t[2]                = { 0x39, 0 };
  char data[2];
  int i = 0;

  pr_info("interruption handled\n");

  // we have reached the 20 values stored in the FIFO
  // we musth read some data to release space;

  // first read the fifo status;
  i2c_master_send(client, &t[0], 1);
  i2c_master_recv(client, &t[1], 1);

  pr_info("Actual FIFO_CTL = %d\n", ((int) t[1] & 0x1F));

  // now we read some data
  for (i = 0; i < ((int) t[1] & 0x1F); i++) {
    char *d = kzalloc(sizeof(char), GFP_KERNEL);
    i2c_master_send(client, &AXIS_REGISTER[0], 1);
    i2c_master_recv(client, data, 2);
    *d = get_bits(data);
    cb_push_back(dev->buffer, d);
  }

  // print fifo status again
  i2c_master_send(client, &t[0], 1);
  i2c_master_recv(client, &t[1], 1);

  pr_info("Actual FIFO_CTL = %d\n", ((int) t[1] & 0x1F));

  return IRQ_HANDLED;
}

// device
adxl345_device  *init_adxl345_device(struct i2c_client *client) {
  int ret;
  adxl345_device *adxl345_dev = (adxl345_device*) devm_kzalloc(&client->dev, sizeof(adxl345_device),
                         GFP_KERNEL);
  if (!adxl345_dev) {
    pr_err("Something goes wrong during device memory allocation");
    return NULL;
  }

  adxl345_dev->ENABLE_AXIS = 0x32;
  adxl345_dev->buffer = cb_init(BUFFER_SIZE, sizeof(char));

  if (!adxl345_dev->buffer) {
    pr_err("Something goes wrong during buffer memory allocation");
    return NULL;
  }

  init_waitqueue_head(&adxl345_dev->queue);

  /* Initialise la partie miscdevice de adxl345_device */
  adxl345_dev->miscdev.minor  = MISC_DYNAMIC_MINOR;
  adxl345_dev->miscdev.name   = "adxl345_misc";
  adxl345_dev->miscdev.fops   = &adi_adxl345_fops;
  adxl345_dev->miscdev.parent = &client->dev;

  /* S'enregistre auprès du framework misc */
  ret = misc_register(&adxl345_dev->miscdev);

  if (ret < 0) {
    pr_err("Something goes wrong during misc registration");
    return NULL;
  }

  return adxl345_dev;
}

static int adi_adxl345_probe(struct i2c_client *client, const struct i2c_device_id *id) {
  char BW_RATE[2]     = { 0x2C, 10    }; // 0x2C 44 BW_RATE R/W 00001010 Data rate and power mode control
  char DEVID[2]       = { 0, 0        }; // 0x00 0 DEVID R 11100101 Device ID
  char INT_ENABLE[2]  = { 0x2E, 0x2   }; // 0x2E 46 INT_ENABLE R/W 00000000 Interrupt enable control
  char DATA_FORMAT[2] = { 0x31, 0     }; // 0x31 49 DATA_FORMAT R/W 00000000 Data format control
  char FIFO_CTL[2]    = { 0x38, 0xD4  }; // 0x38 56 FIFO_CTL R/W 00000000 FIFO control
  char POWER_CTL[2]   = { 0x2D, 0     }; //0x2D 45 POWER_CTL R/W 00000000 Power-saving features control
  char STANDBYON      =   0;
  char STANDBYOFF     =   0x8;

  // FIFO status 0x39

  int ret;
  adxl345_device *adxl345_dev;


  adxl345_dev = init_adxl345_device(client);

  if (!adxl345_dev)
    return -1;

  i2c_set_clientdata(client, adxl345_dev);

  // Device id
  i2c_master_send(client, DEVID, 1);
  i2c_master_recv(client, &DEVID[1], 1);

  // Entering standby mode
  POWER_CTL[1] = STANDBYON;
  i2c_master_send(client, POWER_CTL, 2);

  // Configuration
  i2c_master_send(client, BW_RATE, 2);
  i2c_master_send(client, INT_ENABLE, 2);
  i2c_master_send(client, DATA_FORMAT, 2);
  i2c_master_send(client, FIFO_CTL, 2);
  i2c_master_send(client, FIFO_CTL, 1);

  // On enregistre le mechanisme d'interruption
  ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
    adxl345_irq_fn, IRQF_ONESHOT, "adi_adxl345", client);

  if (ret == -1)
    return -1;

  // On lit une fois toute la fifo
  readAllTheFifo(client);

  // Leaving standby mode
  POWER_CTL[1] = STANDBYOFF;
  i2c_master_send(client, POWER_CTL, 2);

  // Get POWER_CTL
  i2c_master_send(client, POWER_CTL, 1);
  i2c_master_recv(client, &POWER_CTL[1], 1);

  return 0;
}

static int adi_adxl345_remove(struct i2c_client *client)
{
  char            STANDBYON[2]  = { 0x2D, 0 }; //0x2D 45 POWER_CTL R/W 00000000 Power-saving features control
  adxl345_device *dev           = i2c_get_clientdata(client);

  i2c_master_send(client, STANDBYON, 2);
  i2c_master_send(client, STANDBYON, 1);
  i2c_master_recv(client, &STANDBYON[1], 1);

  misc_deregister(&dev->miscdev);

  return 0;
}

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
