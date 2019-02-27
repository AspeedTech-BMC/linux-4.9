#ifndef __ASPEED_ACRY_H__
#define __ASPEED_ACRY_H__

#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/count_zeros.h>
#include <linux/err.h>
// #include <linux/mpi.h>
#include <linux/fips.h>
#include <linux/dma-mapping.h>
#include <crypto/scatterwalk.h>
#include <crypto/internal/akcipher.h>
#include <crypto/internal/kpp.h>
#include <crypto/internal/rsa.h>
#include <crypto/kpp.h>
#include <crypto/dh.h>
#include <crypto/akcipher.h>
#include <crypto/algapi.h>
#include <crypto/ecdh.h>

/* G6 RSA/ECDH */
#define ASPEED_ACRY_TRIGGER		0x000
#define  ACRY_CMD_RSA_TRIGGER		BIT(0)
#define  ACRY_CMD_DMA_RSA_TRIGGER	BIT(1)
#define ASPEED_ACRY_DMA_CMD		0x048
#define  ACRY_CMD_DMA_SRAM_MODE_ECC	(0x2 << 4)
#define  ACRY_CMD_DMA_SRAM_MODE_RSA	(0x3 << 4)
#define  ACRY_CMD_DMA_SRAM_AHB_CPU	BIT(8)
#define  ACRY_CMD_DMA_SRAM_AHB_ENGINE	0
#define ASPEED_ACRY_DMA_SRC_BASE	0x04C
#define ASPEED_ACRY_DMA_DEST		0x050
#define  DMA_DEST_BASE(x)		(x << 16)
#define  DMA_DEST_LEN(x)		(x)
#define ASPEED_ACRY_RSA_KEY_LEN		0x058
#define  RSA_E_BITS_LEN(x)		(x << 16)
#define  RSA_M_BITS_LEN(x)		(x)
#define ASPEED_ACRY_INT_MASK		0x3F8
#define ASPEED_ACRY_STATUS		0x3FC
#define  ACRY_DMA_ISR			BIT(2)
#define  ACRY_RSA_ISR			BIT(1)
#define  ACRY_ECC_ISR			BIT(0)

#define ASPEED_ACRY_BUFF_SIZE		0x1800

#define ASPEED_ACRY_RSA_MAX_LEN		2048

#define CRYPTO_FLAGS_BUSY 		BIT(1)
#define BYTES_PER_DWORD			4


extern int exp_dw_mapping[512];
extern int mod_dw_mapping[512];
// static int data_dw_mapping[512];
extern int data_byte_mapping[2048];

struct aspeed_acry_dev;

typedef int (*aspeed_acry_fn_t)(struct aspeed_acry_dev *);

/******************************************************************************/
/**
 * aspeed_rsa_key - ASPEED RSA key structure. Keys are allocated in DMA zone.
 * @n           : RSA modulus raw byte stream
 * @e           : RSA public exponent raw byte stream
 * @d           : RSA private exponent raw byte stream
 * @n_sz        : length in bytes of RSA modulus n
 * @e_sz        : length in bytes of RSA public exponent
 * @d_sz        : length in bytes of RSA private exponent
 */
struct aspeed_acry_rsa_key {
	u8 *n;
	u8 *e;
	u8 *d;
	size_t n_sz;
	size_t e_sz;
	size_t d_sz;
	int nm;
	int ne;
	int nd;
};

// struct aspeed_rsa_mpi_key {
// 	MPI n;
// 	MPI e;
// 	MPI d;
// }

struct aspeed_acry_rsa_ctx {
	struct aspeed_acry_dev		*acry_dev;
	// struct rsa_key			key;
	// struct aspeed_rsa_mpi_key	key;
	int 				enc;

	size_t n_sz;
	size_t e_sz;
	size_t d_sz;
	int nm;
	int ne;
	int nd;

	void				*rsa_pub_addr;
	dma_addr_t			rsa_pub_dma_addr;
	void				*rsa_priv_addr;
	dma_addr_t			rsa_priv_dma_addr;
};

/*************************************************************************************/

// struct aspeed_ecdh_ctx {
// 	struct aspeed_acry_dev		*acry_dev;
// 	const u8 			*public_key;
// 	unsigned int 			curve_id;
// 	size_t				n_sz;
// 	u8				private_key[256];
// };

/*************************************************************************************/

struct aspeed_acry_dev {
	void __iomem			*regs;
	struct device			*dev;
	int 				irq;
	struct clk			*rsaclk;
	unsigned long			version;

	struct crypto_queue		queue;
	struct tasklet_struct		done_task;
	bool				is_async;
	spinlock_t			lock;
	aspeed_acry_fn_t		resume;
	unsigned long			flags;

	struct akcipher_request		*akcipher_req;
	void __iomem			*acry_sram;


};


struct aspeed_acry_alg {
	struct aspeed_acry_dev		*acry_dev;
	union {
		struct kpp_alg 		kpp;
		struct akcipher_alg 	akcipher;
	} alg;
};

static inline void
aspeed_acry_write(struct aspeed_acry_dev *crypto, u32 val, u32 reg)
{
	// printk("write : val: %x , reg : %x \n", val, reg);
	writel(val, crypto->regs + reg);
}

static inline u32
aspeed_acry_read(struct aspeed_acry_dev *crypto, u32 reg)
{
#if 0
	u32 val = readl(crypto->regs + reg);
	printk("R : reg %x , val: %x \n", reg, val);
	return val;
#else
	return readl(crypto->regs + reg);
#endif
}

extern int aspeed_acry_rsa_trigger(struct aspeed_acry_dev *acry_dev);

extern int aspeed_register_acry_rsa_algs(struct aspeed_acry_dev *acry_dev);
extern int aspeed_register_acry_kpp_algs(struct aspeed_acry_dev *acry_dev);
extern int aspeed_acry_handle_queue(struct aspeed_acry_dev *acry_dev,
				    struct crypto_async_request *new_areq);
#endif
