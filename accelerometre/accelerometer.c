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

#define DRIVER_AUTHOR "Romain, Margheriti, romain.margheriti@telecom-paristech.fr"
#define DRIVER_DESC "Driver for ADXL345"

static int adi_adxl345_probe(struct i2c_client *client, const struct i2c_device_id *id);
static int adi_adxl345_remove(struct i2c_client *client);

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

struct adi_adxl345_device {
    struct miscdevice          miscdev;
};

ssize_t adi_adxl345_read(struct file *file, char __user *buf, size_t count, loff_t *f_pos)
{
  char DATAX0[2]     = { 0x32, 0 }; // 0x32 50 DATAX0 R 00000000 X-Axis Data 0

  struct miscdevice *miscdev = (struct miscdevice *) file->private_data;
  struct i2c_client *client = to_i2c_client(miscdev->parent);

  i2c_master_send(client, DATAX0, 1);
  i2c_master_recv(client, &DATAX0[1], 1);

  if (count >= 1) {
    put_user(DATAX0[1], buf);
    return 1;
  }
  return 0;
}

static const struct file_operations adi_adxl345_fops = {
    .owner			= THIS_MODULE,
    .read       = adi_adxl345_read
};

static int adi_adxl345_probe(struct i2c_client *client, const struct i2c_device_id *id) {
  char DEVID[2]       = { 0, 0 }; // 0x00 0 DEVID R 11100101 Device ID
  char POWER_CTL[2]   = { 0x2D, 0 }; //0x2D 45 POWER_CTL R/W 00000000 Power-saving features control
  char STANDBYON      =   0;
  char STANDBYOFF     =   0x8;
  char BW_RATE[2]     = { 0x2C, 0 }; // 0x2C 44 BW_RATE R/W 00001010 Data rate and power mode control
  char INT_ENABLE[2]  = { 0x2E, 0 }; // 0x2E 46 INT_ENABLE R/W 00000000 Interrupt enable control
  char DATA_FORMAT[2] = { 0x31, 0 }; // 0x31 49 DATA_FORMAT R/W 00000000 Data format control
  char FIFO_CTL[2]    = { 0x38, 0 }; // 0x38 56 FIFO_CTL R/W 00000000 FIFO control

  struct adi_adxl345_device *adi_adxl345_dev;
  int ret;

  adi_adxl345_dev = (struct adi_adxl345_device*) devm_kzalloc(&client->dev, sizeof(struct adi_adxl345_device),
                         GFP_KERNEL);
   if (!adi_adxl345_dev)
       return -ENOMEM;

  /* Initialise la structure foo_device, par exemple avec les
     informations issues du Device Tree */
  // adi_adxl345_dev->i2c_cl      = *client;
  // adi_adxl345_dev->i2c_dev_id  = *id;

  /* Initialise la partie miscdevice de foo_device */
  adi_adxl345_dev->miscdev.minor  = MISC_DYNAMIC_MINOR;
  adi_adxl345_dev->miscdev.name   = "adi_adxl345_misc";
  adi_adxl345_dev->miscdev.fops   = &adi_adxl345_fops;
  adi_adxl345_dev->miscdev.parent = &client->dev;

  i2c_set_clientdata(client, adi_adxl345_dev);

  /* S'enregistre auprès du framework misc */
  ret = misc_register(&adi_adxl345_dev->miscdev);

  // Device id
  i2c_master_send(client, DEVID, 1);
  i2c_master_recv(client, &DEVID[1], 1);

  // Entering standby mode
  POWER_CTL[1] = STANDBYON;
  i2c_master_send(client, POWER_CTL, 2);

  i2c_master_send(client, BW_RATE, 2);
  i2c_master_send(client, INT_ENABLE, 2);
  // Configuration
  i2c_master_send(client, DATA_FORMAT, 2);
  i2c_master_send(client, FIFO_CTL, 2);
  i2c_master_send(client, FIFO_CTL, 1);
  // Leaving standby mode
  POWER_CTL[1] = STANDBYOFF;
  i2c_master_send(client, POWER_CTL, 2);

  // Get POWER_CTL
  i2c_master_send(client, POWER_CTL, 1);
  i2c_master_recv(client, &POWER_CTL[1], 1);

  pr_info("DEVID = %hhx, POWER_CTL = %hhx\n", DEVID[1], POWER_CTL[1]);
  return 0;
}

static int adi_adxl345_remove(struct i2c_client *client)
{
  char STANDBYON[2]   = { 0x2D, 0 }; //0x2D 45 POWER_CTL R/W 00000000 Power-saving features control

  i2c_master_send(client, STANDBYON, 2);
  i2c_master_send(client, STANDBYON, 1);
  i2c_master_recv(client, &STANDBYON[1], 1);

  //misc_deregister(&adi_adxl345_dev->miscdev);

  pr_info("adi_adxl345 remove, POWER_CTL = %hhx\n", STANDBYON[1]);
  return 0;
}

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
