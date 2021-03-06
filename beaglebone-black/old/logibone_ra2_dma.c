#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/ioctl.h>
#include <asm/uaccess.h>   /* copy_to_user */
#include <asm/io.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/memory.h>
#include <linux/gpio.h>
#include <linux/dma-mapping.h>
#include <linux/edma.h>
#include <linux/platform_data/edma.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/i2c.h>
//device tree support
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_dma.h>
#include <linux/of_gpio.h>
#include <linux/of_i2c.h>


#define SSI_CLK 110
#define SSI_DATA 112
#define SSI_DONE 3
#define SSI_PROG 5
#define SSI_INIT 2
#define MODE0	0
#define MODE1 1


#define FPGA_BASE_ADDR	 0x01000000
#define MEM_SIZE	 131072

#define DEVICE_NAME "logibone"

#define I2_IO_EXP_ADDR	0x24
#define I2C_IO_EXP_CONFIG_REG	0x03
#define I2C_IO_EXP_IN_REG	0x00
#define I2C_IO_EXP_OUT_REG	0x01


#define MAX_DMA_TRANSFER_IN_BYTES   (32768)
dma_addr_t dmaphysbuf = 0;
dma_addr_t dmapGpmcbuf = FPGA_BASE_ADDR;
static volatile int irqraised1 = 0;



static char * gDrvrName = DEVICE_NAME;
static unsigned char gDrvrMajor = 0 ;

unsigned char * readBuffer ;
unsigned char * writeBuffer ;


static int LOGIBONE_dm_open(struct inode *inode, struct file *filp);
static int LOGIBONE_dm_release(struct inode *inode, struct file *filp);
static ssize_t LOGIBONE_dm_write(struct file *filp, const char *buf, size_t count,
                       loff_t *f_pos);
static ssize_t LOGIBONE_dm_read(struct file *filp, char *buf, size_t count, loff_t *f_pos);
static long LOGIBONE_dm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
int edma_memtomemcpy(int count, unsigned long src_addr, unsigned long trgt_addr, int dma_ch);
int edma_memtomemcpy_with_burst(int count, unsigned long src_addr, unsigned long trgt_addr, int dma_ch);
static void dma_callback(unsigned lch, u16 ch_status, void *data);



static struct i2c_board_info io_exp_info= {
	I2C_BOARD_INFO("fpga_ctrl", I2_IO_EXP_ADDR),
};

static struct file_operations LOGIBONE_dm_ops = {
    .read =   LOGIBONE_dm_read,
    .write =  LOGIBONE_dm_write,
    .compat_ioctl =  LOGIBONE_dm_ioctl,
    .unlocked_ioctl = LOGIBONE_dm_ioctl,
    .open =   LOGIBONE_dm_open,
    .release =  LOGIBONE_dm_release,
};

enum logibone_type{
	prog,
	mem
};

struct logibone_prog{
	struct i2c_client * i2c_io;
};


struct logibone_mem{
	unsigned short * base_addr ;
	unsigned short * virt_addr ;
	unsigned char * dma_buf ;
	int dma_chan ;
};

union logibone_data{
	struct logibone_prog prog;
	struct logibone_mem mem;
};

struct logibone_device{
	enum logibone_type type ;
	union logibone_data data ;
	struct cdev cdev;
	unsigned char opened ;
};


struct device * prog_device ;
struct class * logibone_class ;
struct logibone_device * logibone_devices ;


int loadBitFile(struct i2c_client * io_cli, const unsigned char * bitBuffer_user, unsigned int length);




#define SSI_DELAY 1


inline void __delay_cycles(unsigned long cycles){
	while(cycles != 0){
		cycles -- ;	
	}
}

volatile unsigned * gpio_regs ;

#define GPIO3_BASE 0x481AE000
#define GPIO3_SETDATAOUT *(gpio_regs+1)
#define GPIO3_CLEARDATAOUT *(gpio_regs)
inline void ssiSetClk(void){
	//gpio_set_value(SSI_CLK, 1);
	GPIO3_SETDATAOUT = (1 << 14) ;
}

inline void ssiClearClk(void){
	//gpio_set_value(SSI_CLK, 0);
	GPIO3_CLEARDATAOUT = (1 << 14);
}

inline void ssiSetData(void){
	//gpio_set_value(SSI_DATA, 1);
	GPIO3_SETDATAOUT= (1 << 16) ;
}

inline void ssiClearData(void){
	//gpio_set_value(SSI_DATA, 0);
	GPIO3_CLEARDATAOUT = (1 << 16);
}

inline void serialConfigWriteByte(unsigned char val){
	unsigned char bitCount = 0 ;
	unsigned char valBuf = val ;
	for(bitCount = 0 ; bitCount < 8 ; bitCount ++){
		ssiClearClk();
		if((valBuf & 0x80) != 0){
			ssiSetData();
		}else{
			ssiClearData();
		}
		//__delay_cycles(SSI_DELAY);	
		ssiSetClk();
		valBuf = (valBuf << 1);
		//__delay_cycles(SSI_DELAY);			
	}
}




inline void i2c_set_pin(struct i2c_client * io_cli, unsigned char pin, unsigned char val){
	unsigned char i2c_buffer [2] ;
	i2c_buffer[0] = I2C_IO_EXP_OUT_REG;
	i2c_master_send(io_cli, i2c_buffer, 1); 
	i2c_master_recv(io_cli, &i2c_buffer[1], 1);
	if(val == 1){
		i2c_buffer[1] |= (1 << pin);	
	}else{
		i2c_buffer[1] &= ~(1 << pin);
	}
	i2c_master_send(io_cli, i2c_buffer, 2); 
}

inline unsigned char i2c_get_pin(struct i2c_client * io_cli, unsigned char pin){
	unsigned char i2c_buffer ;
	i2c_buffer = I2C_IO_EXP_IN_REG;
	i2c_master_send(io_cli, &i2c_buffer, 1); 
	i2c_master_recv(io_cli, &i2c_buffer, 1); 
	//printk("reading value %x \n", i2c_buffer);
	return ((i2c_buffer >> pin) & 0x01) ;
}

int loadBitFile(struct i2c_client * io_cli, const unsigned char * bitBuffer_user, const unsigned int length){
	unsigned char cfg = 1 ;	
	unsigned long int i ;
	unsigned long int timer = 0;
	unsigned char * bitBuffer ;	
	unsigned char i2c_buffer [4] ;
	//request_mem_region(GPIO3_BASE + 0x190, 8, gDrvrName);
	gpio_regs = ioremap_nocache(GPIO3_BASE + 0x190, 2*sizeof(int));

	bitBuffer = kmalloc(length, GFP_KERNEL);
	if(bitBuffer == NULL || copy_from_user(bitBuffer, bitBuffer_user, length)){
		printk("Failed allocate buffer for configuration file \n");
		return -ENOTTY;	
	}

	cfg = gpio_request(SSI_CLK, "ssi_clk");
	if(cfg < 0){
		printk("Failed to take control over ssi_clk pin \n");
		return -ENOTTY;
	}
	cfg = gpio_request(SSI_DATA, "ssi_data");
	if(cfg < 0){
		printk("Failed to take control over ssi_data pin \n");
		return -ENOTTY;
	}
	i2c_buffer[0] = I2C_IO_EXP_CONFIG_REG;
	i2c_buffer[1] = 0xFF;
	i2c_buffer[1] &= ~ ((1 << SSI_PROG) | (1 << MODE1) | (1 << MODE0));
	i2c_master_send(io_cli, i2c_buffer, 2); // set SSI_PROG, MODE0, MODE1 as output others as inputs
	i2c_set_pin(io_cli, MODE0, 1);
	i2c_set_pin(io_cli, MODE1, 1);
	i2c_set_pin(io_cli, SSI_PROG, 0);

	gpio_direction_output(SSI_CLK, 0);
	gpio_direction_output(SSI_DATA, 0);

	
	gpio_set_value(SSI_CLK, 0);
	i2c_set_pin(io_cli, SSI_PROG, 1);
	__delay_cycles(10*SSI_DELAY);	
	i2c_set_pin(io_cli, SSI_PROG, 0);
	__delay_cycles(5*SSI_DELAY);		
	while(i2c_get_pin(io_cli, SSI_INIT) > 0 && timer < 200) timer ++; // waiting for init pin to go down
	if(timer >= 200){
		printk("FPGA did not answer to prog request, init pin not going low \n");
		i2c_set_pin(io_cli, SSI_PROG, 1);
		gpio_free(SSI_CLK);
		gpio_free(SSI_DATA);
		return -ENOTTY;	
	}
	timer = 0;
	__delay_cycles(5*SSI_DELAY);
	i2c_set_pin(io_cli, SSI_PROG, 1);
	while(i2c_get_pin(io_cli, SSI_INIT) == 0 && timer < 256){ // need to find a better way ...
		 timer ++; // waiting for init pin to go up
	}
	if(timer >= 256){
		printk("FPGA did not answer to prog request, init pin not going high \n");
		gpio_free(SSI_CLK);
		gpio_free(SSI_DATA);
		return -ENOTTY;	
	}
	timer = 0;
	printk("Starting configuration of %d bits \n", length*8);
	for(i = 0 ; i < length ; i ++){
		serialConfigWriteByte(bitBuffer[i]);	
		schedule();
	}
	printk("Waiting for done pin to go high \n");
	while(timer < 50){
		ssiClearClk();
		__delay_cycles(SSI_DELAY);	
		ssiSetClk();
		__delay_cycles(SSI_DELAY);	
		timer ++ ;
	}
	gpio_set_value(SSI_CLK, 0);
	gpio_set_value(SSI_DATA, 1);	
	if(i2c_get_pin(io_cli, SSI_DONE) == 0){
		printk("FPGA prog failed, done pin not going high \n");
		gpio_free(SSI_CLK);
		gpio_free(SSI_DATA);
		return -ENOTTY;		
	}

	i2c_buffer[0] = I2C_IO_EXP_CONFIG_REG;
	i2c_buffer[1] = 0xDC;
	i2c_master_send(io_cli, i2c_buffer, 2); // set all unused config pins as input (keeping mode pins and PROG as output)
	gpio_direction_input(SSI_CLK);
	gpio_direction_input(SSI_DATA);
	gpio_free(SSI_CLK);
	gpio_free(SSI_DATA);
	iounmap(gpio_regs);
	//release_mem_region(GPIO3_BASE + 0x190, 8);
	kfree(bitBuffer) ;
	return length ;
}



ssize_t writeMem(struct file *filp, const char *buf, size_t count,
                       loff_t *f_pos)
{

	unsigned short int transfer_size  ;
	ssize_t transferred = 0 ;
	unsigned long src_addr, trgt_addr ;
	unsigned int ret = 0;
	struct logibone_mem * mem_to_write = &(((struct logibone_device *) filp->private_data)->data.mem) ;
	if(count%2 != 0){
		 printk("%s: LOGIBONE_fifo write: Transfer must be 16bits aligned.\n",gDrvrName);
		 return -1;
	}
	if(count < MAX_DMA_TRANSFER_IN_BYTES){
		transfer_size = count ;
	}else{
		transfer_size = MAX_DMA_TRANSFER_IN_BYTES ;
	}
	//writeBuffer =  (unsigned char *) dma_alloc_coherent (NULL, MAX_DMA_TRANSFER_IN_BYTES, &dmaphysbuf, 0);
	writeBuffer = mem_to_write->dma_buf ;
	if(writeBuffer == NULL){
		printk("failed to allocate DMA buffer \n");
		return -1 ;	
	}
	trgt_addr = (unsigned long) &(mem_to_write->base_addr[(*f_pos)/2]) ;
	src_addr = (unsigned long) dmaphysbuf ;

	if (copy_from_user(writeBuffer, buf, transfer_size) ) {
		ret = -1;
		goto exit;
	}
	while(transferred < count){
		if(edma_memtomemcpy(transfer_size, src_addr , trgt_addr,  mem_to_write->dma_chan) < 0){
			printk("%s: LOGIBONE_fifo write: Failed to trigger EDMA transfer.\n",gDrvrName);		
			ret = -1 ;			
			goto exit;		
		}
		trgt_addr += transfer_size ;
		transferred += transfer_size ;
		if((count - transferred) < MAX_DMA_TRANSFER_IN_BYTES){
			transfer_size = count - transferred ;
		}else{
			transfer_size = MAX_DMA_TRANSFER_IN_BYTES ;
		}
		if (copy_from_user(writeBuffer, &buf[transferred], transfer_size) ) {
			ret = -1;
			goto exit;
		}	
	}
	ret = transferred;
	exit:
	/*dma_free_coherent(NULL, MAX_DMA_TRANSFER_IN_BYTES, writeBuffer,
				dmaphysbuf);*/// free coherent before copy to user

	return (ret);
}


ssize_t readMem(struct file *filp, char *buf, size_t count, loff_t *f_pos)
{
	unsigned short int transfer_size ;
	ssize_t transferred = 0 ;
	unsigned long src_addr, trgt_addr ;
	int ret = 0 ;
	struct logibone_mem * mem_to_read = &(((struct logibone_device *) filp->private_data)->data.mem) ;

	if(count%2 != 0){
		 printk("%s: LOGIBONE_fifo read: Transfer must be 16bits aligned.\n",gDrvrName);
		 return -1 ;
	}
	if(count < MAX_DMA_TRANSFER_IN_BYTES){
		transfer_size = count ;
	}else{
		transfer_size = MAX_DMA_TRANSFER_IN_BYTES ;
	}
	readBuffer = mem_to_read->dma_buf; //(unsigned char *) dma_alloc_coherent (NULL, MAX_DMA_TRANSFER_IN_BYTES, &dmaphysbuf, 0);

		
	if(readBuffer == NULL){
		printk("failed to allocate DMA buffer \n");
		return -1 ;	
	}
	src_addr = (unsigned long) &(mem_to_read->base_addr[(*f_pos)/2]);
	trgt_addr = (unsigned long) dmaphysbuf ;
	while(transferred < count){
		if(transfer_size > 32){ // handling 32 bytes alignment
			transfer_size = (transfer_size & 0xFFFFFFE0) ; // masking to get 32 byte factor		
		}
		if(edma_memtomemcpy_with_burst(transfer_size, src_addr, trgt_addr,  mem_to_read->dma_chan) < 0){
		
			printk("%s: LOGIBONE_fifo read: Failed to trigger EDMA transfer.\n",gDrvrName);
			goto exit ;
		}	
		if (copy_to_user(&buf[transferred], readBuffer, transfer_size)){
			ret = -1;
			goto exit;
		}
		src_addr += transfer_size ;
		transferred += transfer_size ;
		if((count - transferred) < MAX_DMA_TRANSFER_IN_BYTES){
			transfer_size = (count - transferred) ;
		}else{
			transfer_size = MAX_DMA_TRANSFER_IN_BYTES ;
		}
	}
	/*dma_free_coherent(NULL,  MAX_DMA_TRANSFER_IN_BYTES, readBuffer,
			dmaphysbuf);*/
	ret = transferred ;
	exit:
	return ret;
}



static int LOGIBONE_dm_open(struct inode *inode, struct file *filp)
{
	struct logibone_device * dev ;
	struct logibone_mem * mem_dev ;
	dev = container_of(inode->i_cdev, struct logibone_device, cdev);
	filp->private_data = dev; /* for other methods */
	if(dev == NULL){
		printk("Failed to retrieve logibone structure !\n");
		return -1 ;	
	}
	if(dev->opened == 1){
		printk("%s: module already opened\n", gDrvrName);
		return 0 ;
	}

	if(dev->type == prog){
		
	}else{
		mem_dev = &((dev->data).mem) ;
		request_mem_region((unsigned long) mem_dev->base_addr, MEM_SIZE, gDrvrName);
		mem_dev->virt_addr = ioremap_nocache(((unsigned long) mem_dev->base_addr), MEM_SIZE);
		mem_dev->dma_chan = edma_alloc_channel (EDMA_CHANNEL_ANY, dma_callback, NULL, EVENTQ_0);
		mem_dev->dma_buf = (unsigned char *) dma_alloc_coherent (NULL, MAX_DMA_TRANSFER_IN_BYTES, &dmaphysbuf, 0);
		printk("EDMA channel %d reserved \n", mem_dev->dma_chan);			
		if (mem_dev->dma_chan < 0) {
			printk ("\nedma3_memtomemcpytest_dma::edma_alloc_channel failed for dma_ch, error:%d\n", mem_dev->dma_chan);
			return -1;
		}

		printk("mem interface opened \n");	


	}
	dev->opened = 1 ;
	return 0;
}

static int LOGIBONE_dm_release(struct inode *inode, struct file *filp)
{
	struct logibone_device * dev ;
	dev = container_of(inode->i_cdev, struct logibone_device, cdev);
	if(dev->opened == 0){
		printk("%s: module already released\n", gDrvrName);
		return 0 ;
	}
	if(dev->type == mem){
		iounmap((dev->data.mem).virt_addr);
		release_mem_region(((unsigned long) (dev->data.mem).base_addr), MEM_SIZE );
		printk("%s: Release: module released\n",gDrvrName);
		dma_free_coherent(NULL,  MAX_DMA_TRANSFER_IN_BYTES, (dev->data.mem).dma_buf, dmaphysbuf);
		edma_free_channel((dev->data.mem).dma_chan);
	}
	dev->opened = 0 ;
	return 0;
}


static ssize_t LOGIBONE_dm_write(struct file *filp, const char *buf, size_t count,
                       loff_t *f_pos)
{
	struct logibone_device * dev ;
	dev = filp->private_data ; /* for other methods */
	switch(dev->type){
		case prog :
			
			return loadBitFile((dev->data.prog.i2c_io), buf, count);
		case mem:
			return writeMem(filp, buf, count, f_pos);
		default:
			return loadBitFile((dev->data.prog.i2c_io), buf, count);	
	};
	
}


static ssize_t LOGIBONE_dm_read(struct file *filp, char *buf, size_t count, loff_t *f_pos)
{
	struct logibone_device * dev ;
	dev = filp->private_data ; /* for other methods */
	switch(dev->type){
		case prog :
			return -1;
		case mem:
			return readMem(filp, buf, count, f_pos);
		default:
			return -1 ;	
	};
}


static long LOGIBONE_dm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg){
	printk("ioctl failed \n");
	return -ENOTTY;
}



static void LOGIBONE_dm_exit(void)
{
	int i;
	dev_t devno = MKDEV(gDrvrMajor, 0);
	/* Get rid of our char dev entries */
	if (logibone_devices) {
		for (i = 0; i < 2; i++) {
			if(i == 0){
				i2c_unregister_device(logibone_devices[i].data.prog.i2c_io);			
			}
			device_destroy(logibone_class, MKDEV(gDrvrMajor, i));
			cdev_del(&logibone_devices[i].cdev);
		}
		kfree(logibone_devices);
	}
	class_destroy(logibone_class);
	/* cleanup_module is never called if registering failed */
	unregister_chrdev_region(devno, 2);
}

static int LOGIBONE_dm_init(void)
{
		
	int result;
	int devno ;
	struct logibone_mem * memDev ;
	struct logibone_prog * progDev ;
	struct i2c_adapter *i2c_adap;
        dev_t dev = 0;
	result = alloc_chrdev_region(&dev, 0, 2,
                                gDrvrName);
        gDrvrMajor = MAJOR(dev);
        if (result < 0) {
                 printk(KERN_ALERT "Registering char device failed with %d\n", gDrvrMajor);
                return result;
        }
        logibone_devices = kmalloc(2 * sizeof(struct logibone_device), GFP_KERNEL);
        if (! logibone_devices) {
                result = -ENOMEM;
                goto fail;  /* Make this more graceful */
        }

	logibone_class = class_create(THIS_MODULE,DEVICE_NAME);

        memset(logibone_devices, 0, 2 * sizeof(struct logibone_device));
	
	/*Initializing main mdevice for prog*/
	devno = MKDEV(gDrvrMajor, 0);
	logibone_devices[0].type = prog ;
	progDev = &(logibone_devices[0].data.prog);
	prog_device = device_create(logibone_class, NULL, devno, NULL, DEVICE_NAME);	// should create /dev entry for main node
	logibone_devices[0].opened = 0 ;
	
	/*
	Do the i2c stuff
	*/
	i2c_adap = i2c_get_adapter(1); // todo need to check i2c adapter id
	if(i2c_adap == NULL){
		printk("Cannot get adapter 1 \n");
		goto fail ;
	}
	progDev->i2c_io = i2c_new_device(i2c_adap , &io_exp_info);
	i2c_put_adapter(i2c_adap); //don't know what it does, seems to release the adapter ...
	
	if(prog_device == NULL){
		class_destroy(logibone_class);
   	 	result = -ENOMEM;
		logibone_devices[0].opened = 0 ;
                goto fail;
	}
	cdev_init(&(logibone_devices[0].cdev), &LOGIBONE_dm_ops);
	logibone_devices[0].cdev.owner = THIS_MODULE;
	logibone_devices[0].cdev.ops = &LOGIBONE_dm_ops;
	cdev_add(&(logibone_devices[0].cdev), devno, 1);
	//printk(KERN_INFO "'mknod /dev/%s c %d %d'.\n", gDrvrName, gDrvrMajor, 0);
	/* Initialize each device. */
	devno = MKDEV(gDrvrMajor, 1);
	logibone_devices[1].type = mem ;
	memDev = &(logibone_devices[1].data.mem);
	memDev->base_addr = (unsigned short *) (FPGA_BASE_ADDR);
	device_create(logibone_class, prog_device, devno, NULL, "logibone_mem"); 
	cdev_init(&(logibone_devices[1].cdev), &LOGIBONE_dm_ops);
	(logibone_devices[1].cdev).owner = THIS_MODULE;
	(logibone_devices[1].cdev).ops = &LOGIBONE_dm_ops;
	cdev_add(&(logibone_devices[1].cdev), devno, 1);
	logibone_devices[1].opened = 0 ;
	return 0 ;
  fail:
        LOGIBONE_dm_exit();
        return -1 ;

}


int edma_memtomemcpy(int count, unsigned long src_addr, unsigned long trgt_addr, int dma_ch)
{
	int result = 0;
	struct edmacc_param param_set;

	edma_set_src (dma_ch, src_addr, INCR, W16BIT);
	edma_set_dest (dma_ch, trgt_addr, INCR, W16BIT);
	edma_set_src_index (dma_ch, 1, 1); 
	//edma_set_src_index (dma_ch, 32, 1);
	edma_set_dest_index (dma_ch, 1, 1); 
	//edma_set_dest_index (dma_ch, 32, 1);
	/* A Sync Transfer Mode */
	edma_set_transfer_params (dma_ch, count, 1, 1, 1, ASYNC); //one block of one frame of one array of count bytes

	/* Enable the Interrupts on Channel 1 */
	edma_read_slot (dma_ch, &param_set);
	param_set.opt |= ITCINTEN;
	param_set.opt |= TCINTEN;
	param_set.opt |= EDMA_TCC(EDMA_CHAN_SLOT(dma_ch));
	edma_write_slot (dma_ch, &param_set);
	irqraised1 = 0u;
	result = edma_start(dma_ch);
	if (result != 0) {
		printk ("edma copy for logibone_fifo failed \n");
		
	}
	while (irqraised1 == 0u) schedule();
	//irqraised1 = 0u;
	//irqraised1 = -1 ;

	/* Check the status of the completed transfer */
	if (irqraised1 < 0) {
		printk ("edma copy for logibone_fifo: Event Miss Occured!!!\n");
	}
	edma_stop(dma_ch);
	return result;
}


int edma_memtomemcpy_with_burst(int count, unsigned long src_addr, unsigned long trgt_addr, int dma_ch)
{
	int result = 0;
	unsigned int a_count, b_count ;
	struct edmacc_param param_set;

	a_count = (count > 32 ) ? 32 : count ;
	b_count = (count > 32 ) ? count/32 : 1 ;
	edma_set_src (dma_ch, src_addr, INCR, W16BIT);
	edma_set_dest (dma_ch, trgt_addr, INCR, W16BIT);
	//edma_set_src_index (dma_ch, 1, 1); 
	edma_set_src_index (dma_ch, a_count, 1);
	//edma_set_dest_index (dma_ch, 1, 1); 
	edma_set_dest_index (dma_ch, a_count, 1);
	/* A Sync Transfer Mode */
	edma_set_transfer_params (dma_ch, a_count, b_count, 1, 1, ASYNC); //one block of one frame of one array of count bytes

	/* Enable the Interrupts on Channel 1 */
	edma_read_slot (dma_ch, &param_set);
	param_set.opt |= ITCINTEN;
	param_set.opt |= TCINTEN;
	param_set.opt |= EDMA_TCC(EDMA_CHAN_SLOT(dma_ch));
	edma_write_slot (dma_ch, &param_set);
	irqraised1 = 0u;
	result = edma_start(dma_ch);
	if (result != 0) {
		printk ("edma copy for logibone_fifo failed \n");
		
	}
	while (irqraised1 == 0u) udelay(5);//schedule();
	//irqraised1 = 0u;
	//irqraised1 = -1 ;

	/* Check the status of the completed transfer */
	if (irqraised1 < 0) {
		printk ("edma copy for logibone_fifo: Event Miss Occured!!!\n");
	}
	edma_stop(dma_ch);
	return result;
}


static void dma_callback(unsigned lch, u16 ch_status, void *data)
{	
	switch(ch_status) {
	case DMA_COMPLETE:
		irqraised1 = 1;
		break;
	case DMA_CC_ERROR:
		irqraised1 = -1;
		break;
	default:
		irqraised1 = -1;
		break;
	}
}





static const struct of_device_id logibone_of_match[] = {
	{ .compatible = "logibone", },
	{ },
};
MODULE_DEVICE_TABLE(of, logibone_of_match);


MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Jonathan Piat <piat.jonathan@gmail.com>");

module_init(LOGIBONE_dm_init);
module_exit(LOGIBONE_dm_exit);
