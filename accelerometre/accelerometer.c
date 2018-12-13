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

#define BUFFER_SIZE 64

typedef struct circular_buffer_t {
  int  first, last, size, max_size;
  char *buffer;
} circular_buffer;


void circular_buffer_info(circular_buffer *cb) {
  int i;
  char *tmp = cb->buffer;
  
  pr_info("The buffer has %d * %d elements", cb->max_size, sizeof(char));
  for (i = 0; i < cb->max_size; i++) {
    pr_info("%d: addr = %p, value = %hhx\n", i, tmp, *tmp);
    tmp++;
  }
}

circular_buffer *circular_buffer_init(int max_size) {
    circular_buffer * b = (circular_buffer *)kzalloc(sizeof(circular_buffer), GFP_KERNEL);
    b->first = 0;
    b->last  = -1;
    b->size = 0;
    b->max_size = max_size;
    b->buffer = (char *)kzalloc(max_size*sizeof(char), GFP_KERNEL);
    return b;
}

char *circular_buffer_get(circular_buffer *b, size_t size){
  int i;
  char *d = kzalloc(size, GFP_KERNEL);
  
  if (b->size == 0) return NULL;

  for (i = 0; i < size; i++) {
    d[i] = b->buffer[b->first];
    b->first = (b-> first + 1) % b->max_size;
    b->size--;
  }
  return d;
}

char *circular_buffer_read(circular_buffer *b) {
  if (b->size == 0) return NULL;
  return &b->buffer[b->first];
}


int circular_buffer_put(circular_buffer *b, char *c, size_t size) {
  int i;
  
  for (i = 0; i < size; i++) { 

    if (b->size == b->max_size) {
      pr_info("Cannot add data to buffer");
      return 0;
    }

    b->last = (b->last + 1) % b->max_size;
    b->buffer[b->last] = c[i];
    b->size++;
  }
  return 1;
}

int circular_buffer_size(circular_buffer *b) {
  return b->size;
}

#define DRIVER_AUTHOR "Romain, Margheriti, romain.margheriti@telecom-paristech.fr"
#define DRIVER_DESC   "Driver for ADXL345"

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
  struct miscdevice *miscdev    = (struct miscdevice *) file->private_data;
  struct i2c_client *client     = to_i2c_client(miscdev->parent);
  adxl345_device    *dev        = i2c_get_clientdata(client);
  int                buff_size  = circular_buffer_size(dev->buffer);

  // test if buffer is empty
  if (buff_size == 0)
    wait_event(dev->queue, ((buff_size = circular_buffer_size(dev->buffer) > 0)));

  if (count > buff_size) {
    // user ask for more data then we have
    copy_to_user(buf, circular_buffer_get(dev->buffer, buff_size), buff_size);

    return buff_size;
  } else {
    // user ask for less data then max we have
    copy_to_user(buf, circular_buffer_get(dev->buffer, count), count);

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

  pr_info("Actual FIFO_CTL = %hhx\n", t[1]);
  
  char *d = kzalloc(sizeof(char) * (int) (t[1] & 0x3F), GFP_KERNEL);
  
  // now we read some data
  for (i = 0; i < (t[1] & 0x3F); i++) {
    i2c_master_send(client, &AXIS_REGISTER[0], 1);
    i2c_master_recv(client, data, 2);
    d[i] = ((data[0]>>2) & 0x3f) | data[1] << 6; 
  }

  circular_buffer_put(dev->buffer, d, (int) (t[1] & 0x3F));

  circular_buffer_info(dev->buffer);

  kfree(d);
  
  return IRQ_HANDLED;
}

// device
adxl345_device  *init_adxl345_device(struct i2c_client *client) {
  int ret;
  adxl345_device *adxl345_dev = (adxl345_device*) devm_kzalloc(&client->dev, sizeof(adxl345_device), GFP_KERNEL);
  if (!adxl345_dev) {
    pr_err("Something goes wrong during device memory allocation");
    return NULL;
  }

  adxl345_dev->ENABLE_AXIS = 0x32;
  adxl345_dev->buffer = circular_buffer_init(BUFFER_SIZE);

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

  circular_buffer_info(adxl345_dev->buffer);
  
  return adxl345_dev;
}

static int adi_adxl345_probe(struct i2c_client *client, const struct i2c_device_id *id) {
  char BW_RATE[2]     = { 0x2C, 10    }; // 0x2C 44 BW_RATE R/W 00001010 Data rate and power mode control
  char DEVID[2]       = { 0, 0        }; // 0x00 0 DEVID R 11100101 Device ID
  char INT_ENABLE[2]  = { 0x2E, 0x2   }; // 0x2E 46 INT_ENABLE R/W 00000000 Interrupt enable control
  char DATA_FORMAT[2] = { 0x31, 0     }; // 0x31 49 DATA_FORMAT R/W 00000000 Data format control
  char FIFO_CTL[2]    = { 0x38, 0x94  }; // 0x38 56 FIFO_CTL R/W 00000000 FIFO control
  char POWER_CTL[2]   = { 0x2D, 0     }; // 0x2D 45 POWER_CTL R/W 00000000 Power-saving features control
  char STANDBYON      =   0;
  char STANDBYOFF     =   0x8;

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
