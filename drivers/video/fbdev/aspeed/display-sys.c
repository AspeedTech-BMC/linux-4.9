#include <linux/module.h>
#include <linux/ctype.h>
#include <linux/idr.h>
#include <linux/err.h>
#include <linux/kdev_t.h>
#include "display-sys.h"

static LIST_HEAD(display_device_list);

static ssize_t display_show_name(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ast_display_device *dsp = dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%s\n", dsp->name);
}

static ssize_t display_show_type(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ast_display_device *dsp = dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%s\n", dsp->type);
}

static ssize_t display_show_enable(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ast_display_device *dsp = dev_get_drvdata(dev);
	int enable;
	if(dsp->ops && dsp->ops->getenable)
		enable = dsp->ops->getenable(dsp);
	else
		return 0;
	return snprintf(buf, PAGE_SIZE, "%d\n", enable);
}

static ssize_t display_store_enable(struct device *dev, 
						struct device_attribute *attr,
			 			const char *buf, size_t size)
{
	struct ast_display_device *dsp = dev_get_drvdata(dev);
	int enable;
	
	sscanf(buf, "%d", &enable);
	if(dsp->ops && dsp->ops->setenable)
		dsp->ops->setenable(dsp, enable);
	return size;
}

static ssize_t display_show_connect(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ast_display_device *dsp = dev_get_drvdata(dev);
	int connect;
	if(dsp->ops && dsp->ops->getstatus)
		connect = dsp->ops->getstatus(dsp);
	else
		return 0;
	return snprintf(buf, PAGE_SIZE, "%d\n", connect);
}

static int mode_string(char *buf, unsigned int offset,
		       const struct fb_videomode *mode)
{
//	char m = 'U';
	char v = 'p';

//	if (mode->flag & FB_MODE_IS_DETAILED)
//		m = 'D';
//	if (mode->flag & FB_MODE_IS_VESA)
//		m = 'V';
//	if (mode->flag & FB_MODE_IS_STANDARD)
//		m = 'S';

	if (mode->vmode & FB_VMODE_INTERLACED)
		v = 'i';
	if (mode->vmode & FB_VMODE_DOUBLE)
		v = 'd';

	return snprintf(&buf[offset], PAGE_SIZE - offset, "%dx%d%c-%d\n",
	                mode->xres, mode->yres, v, mode->refresh);
}
static ssize_t display_show_modes(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ast_display_device *dsp = dev_get_drvdata(dev);
	struct list_head *modelist, *pos;
	struct fb_modelist *fb_modelist;
	const struct fb_videomode *mode;
	int i;
	if(dsp->ops && dsp->ops->getmodelist)
	{
		if(dsp->ops->getmodelist(dsp, &modelist))
			return -EINVAL;
	}
	else
		return 0;

	i = 0;
	list_for_each(pos, modelist) {
		fb_modelist = list_entry(pos, struct fb_modelist, list);
		mode = &fb_modelist->mode;
		i += mode_string(buf, i, mode);
	}
	return i;
}

static ssize_t display_show_mode(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ast_display_device *dsp = dev_get_drvdata(dev);
	struct fb_videomode mode;
	
	if(dsp->ops && dsp->ops->getmode)
		if(dsp->ops->getmode(dsp, &mode) == 0)
			return mode_string(buf, 0, &mode);
	return 0;
}

static ssize_t display_store_mode(struct device *dev, 
						struct device_attribute *attr,
			 			const char *buf, size_t count)
{
	struct ast_display_device *dsp = dev_get_drvdata(dev);
	char mstr[100];
	struct list_head *modelist, *pos;
	struct fb_modelist *fb_modelist;
	struct fb_videomode *mode;     
	size_t i;                   

	if(dsp->ops && dsp->ops->getmodelist)
	{
		if(dsp->ops && dsp->ops->getmodelist)
		{
			if(dsp->ops->getmodelist(dsp, &modelist))
				return -EINVAL;
		}
		list_for_each(pos, modelist) {
			fb_modelist = list_entry(pos, struct fb_modelist, list);
			mode = &fb_modelist->mode;
			i = mode_string(mstr, 0, mode);
			if (strncmp(mstr, buf, max(count, i)) == 0) {
				if(dsp->ops && dsp->ops->setmode)
					dsp->ops->setmode(dsp, mode);
				return count;
			}
		}
	}
	return -EINVAL;
}

static ssize_t display_show_scale(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ast_display_device *dsp = dev_get_drvdata(dev);
	int xscale, yscale;
	
	if(dsp->ops && dsp->ops->getscale) {
		xscale = dsp->ops->getscale(dsp, DISPLAY_SCALE_X);
		yscale = dsp->ops->getscale(dsp, DISPLAY_SCALE_Y);
		if(xscale && yscale)
			return snprintf(buf, PAGE_SIZE, "xscale=%d yscale=%d\n", xscale, yscale);
	}
	return -EINVAL;
}

static ssize_t display_store_scale(struct device *dev, 
						struct device_attribute *attr,
			 			const char *buf, size_t count)
{
	struct ast_display_device *dsp = dev_get_drvdata(dev);
	int scale = 100;
	
	if(dsp->ops && dsp->ops->setscale) {
		if(!strncmp(buf, "xscale", 6)) {
			sscanf(buf, "xscale=%d", &scale);
			dsp->ops->setscale(dsp, DISPLAY_SCALE_X, scale);
		}
		else if(!strncmp(buf, "yscale", 6)) {
			sscanf(buf, "yscale=%d", &scale);
			dsp->ops->setscale(dsp, DISPLAY_SCALE_Y, scale);
		}
		else {
			sscanf(buf, "%d", &scale);
			dsp->ops->setscale(dsp, DISPLAY_SCALE_X, scale);
			dsp->ops->setscale(dsp, DISPLAY_SCALE_Y, scale);
		}
		return count;
	}
	return -EINVAL;
}

static ssize_t display_show_debug(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return -EINVAL;
}

static ssize_t display_store_debug(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int cmd;
	struct ast_display_device *dsp = dev_get_drvdata(dev);

	if(dsp->ops && dsp->ops->setdebug) {
		sscanf(buf, "%d", &cmd);
		dsp->ops->setdebug(dsp, cmd);
		return count;
	}
	return -EINVAL;
}

static ssize_t display_show_sinkaudioinfo(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ast_display_device *dsp = dev_get_drvdata(dev);
	char audioinfo[200];
	int ret=0;

	if(dsp->ops && dsp->ops->getedidaudioinfo) {
		ret = dsp->ops->getedidaudioinfo(dsp, audioinfo, 200);
		if(!ret){
			return snprintf(buf, PAGE_SIZE, "%s\n", audioinfo);
		}
	}
	return -EINVAL;
}



static ssize_t display_show_monspecs(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct ast_display_device *dsp = dev_get_drvdata(dev);
	struct fb_monspecs monspecs;
	int ret = 0;
	if(dsp->ops && dsp->ops->getmonspecs) {
		ret = dsp->ops->getmonspecs(dsp,&monspecs);
		if(!ret) {
			memcpy(buf, &monspecs, sizeof(struct fb_monspecs));
			return sizeof(struct fb_monspecs);//snprintf(buf, PAGE_SIZE, "%s\n", monspecs.monitor);
		}
	}
	return -EINVAL;
}


static struct device_attribute display_attrs[] = {
	__ATTR(name, S_IRUGO, display_show_name, NULL),
	__ATTR(type, S_IRUGO, display_show_type, NULL),
	__ATTR(enable, 0664, display_show_enable, display_store_enable),
	__ATTR(connect, S_IRUGO, display_show_connect, NULL),
	__ATTR(modes, S_IRUGO, display_show_modes, NULL),
	__ATTR(mode, 0664, display_show_mode, display_store_mode),
	__ATTR(scale, 0664, display_show_scale, display_store_scale),
	__ATTR(debug, 0664, display_show_debug, display_store_debug),
	__ATTR(audioinfo, S_IRUGO, display_show_sinkaudioinfo, NULL),
	__ATTR(monspecs, S_IRUGO, display_show_monspecs, NULL),
	__ATTR_NULL
};

static int display_suspend(struct device *dev, pm_message_t state)
{
	struct ast_display_device *dsp = dev_get_drvdata(dev);

	mutex_lock(&dsp->lock);
	if (likely(dsp->driver->suspend))
		dsp->driver->suspend(dsp, state);
	mutex_unlock(&dsp->lock);
	return 0;
};

static int display_resume(struct device *dev)
{
	struct ast_display_device *dsp = dev_get_drvdata(dev);

	mutex_lock(&dsp->lock);
	if (likely(dsp->driver->resume))
		dsp->driver->resume(dsp);
	mutex_unlock(&dsp->lock);
	return 0;
};

void ast_display_device_enable(struct ast_display_device *ddev)
{
//#ifndef CONFIG_DISPLAY_AUTO_SWITCH	
//	return;
//#else
	struct list_head *pos, *head = &display_device_list;
	struct ast_display_device *dev = NULL, *dev_enabled = NULL, *dev_enable = NULL;
	int enable = 0,connect;
	
	list_for_each(pos, head) {
		dev = list_entry(pos, struct ast_display_device, list);
		enable = dev->ops->getenable(dev);
		connect = dev->ops->getstatus(dev);
		if(connect)
			dev_enable = dev;
		if(enable == 1)
			dev_enabled = dev;
	}
	// If no device is connected, enable highest priority device.
	if(dev_enable == NULL) {
		dev->ops->setenable(dev, 1);
		return;
	}
	
	if(dev_enable == dev_enabled) {
		if(dev_enable != ddev)
			ddev->ops->setenable(ddev, 0);
	}
	else {
		if(dev_enabled)
			dev_enabled->ops->setenable(dev_enabled, 0);
		dev_enable->ops->setenable(dev_enable, 1);
	}
		

//#endif
}
EXPORT_SYMBOL(ast_display_device_enable);

void ast_display_device_enable_other(struct ast_display_device *ddev)
{
#ifndef CONFIG_DISPLAY_AUTO_SWITCH	
	return;
#else
	struct list_head *pos, *head = &display_device_list;
	struct ast_display_device *dev;	
	int connect = 0;
	
	list_for_each_prev(pos, head) {
		dev = list_entry(pos, struct ast_display_device, list);
		if(dev != ddev)
		{
			connect = dev->ops->getstatus(dev);
			if(connect)
			{
				dev->ops->setenable(dev, 1);
				return;
			}
		}
	}
#endif
}
EXPORT_SYMBOL(ast_display_device_enable_other);

void ast_display_device_disable_other(struct ast_display_device *ddev)
{
#ifndef CONFIG_DISPLAY_AUTO_SWITCH
	return;
#else
	struct list_head *pos, *head = &display_device_list;
	struct ast_display_device *dev;	
	int enable = 0;
	
	list_for_each(pos, head) {
		dev = list_entry(pos, struct ast_display_device, list);
		if(dev != ddev)
		{
			enable = dev->ops->getenable(dev);
			if(enable)
				dev->ops->setenable(dev, 0);
		}
	}
	ddev->ops->setenable(ddev, 1);
#endif
}
EXPORT_SYMBOL(ast_display_device_disable_other);

struct ast_display_device *ast_display_device_select(int display_dev_no)
{
	struct ast_display_device *disp_dev = NULL;

	list_for_each_entry(disp_dev, &display_device_list, list) {
		if(disp_dev->idx == display_dev_no) {
				return disp_dev;
				break;
		}
	}

	return NULL;
}
EXPORT_SYMBOL(ast_display_device_select);

static struct class *display_class;

struct ast_display_device *ast_display_device_register(struct ast_display_driver *driver,
						struct device *parent, void *devdata)
{
	struct ast_display_device *disp_dev = kzalloc(sizeof(struct ast_display_device), GFP_KERNEL);

	printk("ast_display_device_register no : %d, name %s \n", driver->display_no, driver->name);
	disp_dev->idx = driver->display_no;
	disp_dev->name = driver->name;
	disp_dev->driver = driver;
	driver->probe(disp_dev, devdata);
#if 0	
	if (likely(disp_dev) && unlikely(driver->probe(disp_dev, devdata))) {
		printk("disp_dev->idx %d \n", disp_dev->idx);
		disp_dev->dev = device_create(display_class, parent,
					     MKDEV(0, 0), disp_dev,
					     "%s", disp_dev->type);
		if (!IS_ERR(disp_dev->dev)) {
			disp_dev->parent = parent;
			disp_dev->driver = driver;
			if(parent)
				disp_dev->dev->driver = parent->driver;
			// Add new device to display device list.	
			printk("list add tail \n");
			mutex_init(&disp_dev->lock);				
			list_add_tail(&disp_dev->list, &display_device_list);
		} else {
			printk("ERROR ~~~~~~~~\n");
		}
	}
#else
	list_add_tail(&disp_dev->list, &display_device_list);
#endif
	
}
EXPORT_SYMBOL(ast_display_device_register);

void ast_display_device_unregister(struct ast_display_device *ddev)
{
	if (!ddev)
		return;
	// Free device
	mutex_lock(&ddev->lock);
	device_unregister(ddev->dev);
	mutex_unlock(&ddev->lock);
	list_del(&ddev->list);
	kfree(ddev);
}
EXPORT_SYMBOL(ast_display_device_unregister);
#if 0
static int __init ast_display_class_init(void)
{
	display_class = class_create(THIS_MODULE, "display");
	if (IS_ERR(display_class)) {
		printk(KERN_ERR "Failed to create display class\n");
		display_class = NULL;
		return -EINVAL;
	}
	display_class->dev_attrs = display_attrs;
	display_class->suspend = display_suspend;
	display_class->resume = display_resume;
	INIT_LIST_HEAD(&display_device_list);
	return 0;
}

static void __exit ast_display_class_exit(void)
{
	class_destroy(display_class);
}

#endif
MODULE_AUTHOR("ryan_chen@aspeedtech.com");
MODULE_DESCRIPTION("Driver for ast display device");
MODULE_LICENSE("GPL");
