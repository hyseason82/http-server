/*
 * mydev_i2c.c — I2C 从机驱动框架
 *
 * 演示 Linux I2C 子系统驱动模型：
 *   i2c_driver（驱动逻辑）+ i2c_client（总线上的设备实例）
 *   内核通过 id_table 或 of_match_table 匹配，调用 probe。
 *
 * 模拟一个简单温度传感器（寄存器布局参考 LM75）：
 *   REG_TEMP (0x00)：16-bit 温度值，高 9 位有效，单位 0.5°C/bit
 *   REG_CONF (0x01)：配置寄存器
 *   REG_ID   (0x07)：设备 ID（期望值 0xA5，用于 probe 验证）
 *
 * 测试方法（WSL2，用 i2c-stub 模拟 I2C 控制器）：
 *   sudo modprobe i2c-stub chip_addr=0x48
 *   # 查看 i2c-stub 创建的总线编号（通常是最大编号）
 *   i2cdetect -l
 *   # 预设设备 ID 寄存器（否则 probe 验证失败）
 *   sudo i2cset -y 0 0x48 0x07 0xa5
 *   # 加载驱动，向内核注册设备
 *   sudo insmod mydev_i2c.ko
 *   echo mydev_sensor 0x48 > /sys/bus/i2c/devices/i2c-0/new_device
 *   # 读取 sysfs 温度
 *   cat /sys/bus/i2c/devices/0-0048/temperature_raw
 *
 * 在真实硬件上，设备树节点如下：
 *   &i2c1 {
 *       mysensor: temperature@48 {
 *           compatible = "mydev,sensor";
 *           reg = <0x48>;
 *       };
 *   };
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/of.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hayes");
MODULE_DESCRIPTION("I2C driver framework demo: temperature sensor");

#define REG_TEMP  0x00
#define REG_CONF  0x01
#define REG_ID    0x07
#define EXPECTED_ID 0xA5

/* 每个设备实例的私有数据 */
struct sensor_data {
	struct i2c_client *client;
};

/*
 * I2C 读操作封装：
 *   i2c_smbus_read_byte_data  — 读 1 字节寄存器（最常用）
 *   i2c_smbus_read_word_data  — 读 2 字节寄存器（大端序）
 *   i2c_smbus_write_byte_data — 写 1 字节寄存器
 *   i2c_transfer              — 原始 I2C 报文，适合非标准协议
 */
static int sensor_read_reg(struct i2c_client *client, u8 reg, u16 *val)
{
	int ret = i2c_smbus_read_word_data(client, reg);
	if (ret < 0)
		return ret;
	/* smbus_read_word 返回小端序，LM75 温度寄存器是大端 */
	*val = be16_to_cpu((__be16)ret);
	return 0;
}

/* sysfs 属性：cat /sys/bus/i2c/devices/0-0048/temperature_raw */
static ssize_t temperature_raw_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct sensor_data *data = dev_get_drvdata(dev);
	u16 raw;
	int ret;

	ret = sensor_read_reg(data->client, REG_TEMP, &raw);
	if (ret)
		return ret;

	/*
	 * LM75 温度格式：bit[15:7] 为有效位，bit[0] = 0.5°C
	 * 转为实际温度（单位 0.5°C）：raw >> 7
	 */
	return sysfs_emit(buf, "%d\n", (s16)raw >> 7);
}
static DEVICE_ATTR_RO(temperature_raw);

static struct attribute *sensor_attrs[] = {
	&dev_attr_temperature_raw.attr,
	NULL,
};
static const struct attribute_group sensor_attr_group = {
	.attrs = sensor_attrs,
};

/*
 * probe：内核将此驱动与 I2C 设备匹配后调用。
 * 参数 client 包含设备地址、所在总线、设备名等信息。
 */
static int sensor_probe(struct i2c_client *client)
{
	struct sensor_data *data;
	int id, ret;

	/* 验证 I2C 适配器支持 SMBus 字节读 */
	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_WORD_DATA)) {
		dev_err(&client->dev, "I2C adapter lacks required functionality\n");
		return -ENODEV;
	}

	/* 读设备 ID 寄存器，确认是目标器件 */
	id = i2c_smbus_read_byte_data(client, REG_ID);
	if (id < 0) {
		dev_err(&client->dev, "failed to read device ID: %d\n", id);
		return id;
	}
	if (id != EXPECTED_ID) {
		dev_err(&client->dev, "wrong device ID: got 0x%02x, expected 0x%02x\n",
			id, EXPECTED_ID);
		return -ENODEV;
	}

	/* devm_kzalloc：内存与设备生命周期绑定，remove 时自动释放 */
	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	i2c_set_clientdata(client, data);   /* 存入 client，remove 时用 i2c_get_clientdata 取回 */

	/* 注册 sysfs 属性组（devm 版本自动在 remove 时清理） */
	ret = devm_device_add_group(&client->dev, &sensor_attr_group);
	if (ret)
		return ret;

	dev_info(&client->dev, "mydev_sensor probed at 0x%02x, ID=0x%02x\n",
		 client->addr, id);
	return 0;
}

static void sensor_remove(struct i2c_client *client)
{
	dev_info(&client->dev, "mydev_sensor removed\n");
}

/* id_table：非设备树系统（x86、旧 ARM）通过字符串匹配 */
static const struct i2c_device_id sensor_id[] = {
	{ "mydev_sensor", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, sensor_id);

/* of_match_table：设备树系统通过 compatible 匹配 */
static const struct of_device_id sensor_of_match[] = {
	{ .compatible = "mydev,sensor" },
	{}
};
MODULE_DEVICE_TABLE(of, sensor_of_match);

static struct i2c_driver sensor_driver = {
	.driver = {
		.name           = "mydev_sensor",
		.of_match_table = sensor_of_match,
	},
	.probe    = sensor_probe,
	.remove   = sensor_remove,
	.id_table = sensor_id,
};

/*
 * module_i2c_driver 宏展开为标准的 module_init/module_exit，
 * 自动调用 i2c_add_driver / i2c_del_driver。
 */
module_i2c_driver(sensor_driver);
