//////////////////////////////////////////////////////////////////////
////                                                              ////
////  Interrupt-driven Ethernet MAC test code                     ////
////                                                              ////
////  Description                                                 ////
////  Do ethernet receive path testing                            ////
////  Relies on testbench to provide simulus - expects at least   ////
////  256 packets to be sent.                                     ////
////                                                              ////
////  Author(s):                                                  ////
////      - jb, jb@orsoc.se, with parts taken from Linux kernel   ////
////        open_eth driver.                                      ////
////                                                              ////
////                                                              ////
//////////////////////////////////////////////////////////////////////
////                                                              ////
//// Copyright (C) 2009 Authors and OPENCORES.ORG                 ////
////                                                              ////
//// This source file may be used and distributed without         ////
//// restriction provided that this copyright statement is not    ////
//// removed from the file and that any derivative work contains  ////
//// the original copyright notice and the associated disclaimer. ////
////                                                              ////
//// This source file is free software; you can redistribute it   ////
//// and/or modify it under the terms of the GNU Lesser General   ////
//// Public License as published by the Free Software Foundation; ////
//// either version 2.1 of the License, or (at your option) any   ////
//// later version.                                               ////
////                                                              ////
//// This source is distributed in the hope that it will be       ////
//// useful, but WITHOUT ANY WARRANTY; without even the implied   ////
//// warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR      ////
//// PURPOSE.  See the GNU Lesser General Public License for more ////
//// details.                                                     ////
////                                                              ////
//// You should have received a copy of the GNU Lesser General    ////
//// Public License along with this source; if not, download it   ////
//// from http://www.opencores.org/lgpl.shtml                     ////
////                                                              ////
//////////////////////////////////////////////////////////////////////

#include "or32-utils.h"
#include "spr-defs.h"
#include "board.h"
#include "int.h"
#include "uart.h"
#include "open-eth.h"
#include "printf.h"
#include "eth-phy-mii.h"

volatile unsigned tx_done;
volatile unsigned rx_done;

/* Functions in this file */
void ethmac_setup(void);
void ethphy_init(void);
void oeth_dump_bds();
/* Interrupt functions */
void oeth_interrupt(void);
static void oeth_rx(void);
static void oeth_tx(void);

/* Defining RTLSIM turns off use of real printf'ing to save time in simulation */
#define RTLSIM

#ifdef RTLSIM
#define printk
#else
#define printk printf
#endif
/* Let the ethernet packets use a space beginning here for buffering */
#define ETH_BUFF_BASE 0x01000000


#define RXBUFF_PREALLOC	1
#define TXBUFF_PREALLOC	1
//#undef RXBUFF_PREALLOC
//#undef TXBUFF_PREALLOC

/* The transmitter timeout
 */
#define TX_TIMEOUT	(2*HZ)

/* Buffer number (must be 2^n) 
 */
#define OETH_RXBD_NUM		16
#define OETH_TXBD_NUM		16
#define OETH_RXBD_NUM_MASK	(OETH_RXBD_NUM-1)
#define OETH_TXBD_NUM_MASK	(OETH_TXBD_NUM-1)

/* Buffer size 
 */
#define OETH_RX_BUFF_SIZE	0x600
#define OETH_TX_BUFF_SIZE	0x600

/* Buffer size  (if not XXBUF_PREALLOC 
 */
#define MAX_FRAME_SIZE		1518

/* The buffer descriptors track the ring buffers.   
 */
struct oeth_private {
  //struct	sk_buff* rx_skbuff[OETH_RXBD_NUM];
  //struct	sk_buff* tx_skbuff[OETH_TXBD_NUM];

  unsigned short	tx_next;			/* Next buffer to be sent */
  unsigned short	tx_last;			/* Next buffer to be checked if packet sent */
  unsigned short	tx_full;			/* Buffer ring fuul indicator */
  unsigned short	rx_cur;				/* Next buffer to be checked if packet received */
  
  oeth_regs	*regs;			/* Address of controller registers. */
  oeth_bd		*rx_bd_base;		/* Address of Rx BDs. */
  oeth_bd		*tx_bd_base;		/* Address of Tx BDs. */
  
  //	struct net_device_stats stats;
};


#define PHYNUM 7

/* Scan the MIIM bus for PHYs */
void scan_ethphys(void)
{
  unsigned int phynum,regnum, i;
  
  volatile oeth_regs *regs;
  regs = (oeth_regs *)(OETH_REG_BASE);
  
  regs->miitx_data = 0;
 
  for(phynum=0;phynum<32;phynum++)
    {
      for (regnum=0;regnum<8;regnum++)
	{
	  printk("scan_ethphys: phy %d r%d ",phynum, regnum);
	  
	  /* Now actually perform the read on the MIIM bus*/
	  regs->miiaddress = (regnum << 8) | phynum; /* Basic Control Register */
	  regs->miicommand = OETH_MIICOMMAND_RSTAT;
	  
	  while(!(regs->miistatus & OETH_MIISTATUS_BUSY)); /* Wait for command to be registered*/
	
	  regs->miicommand = 0;
	  
	  while(regs->miistatus & OETH_MIISTATUS_BUSY);
	  
	  printk("%x\n",regs->miirx_data);
	}
    }
}


	  
void ethmac_scanstatus(void)
{
  volatile oeth_regs *regs;
  regs = (oeth_regs *)(OETH_REG_BASE);

  
  printk("Oeth: regs->miistatus %x regs->miirx_data %x\n",regs->miistatus, regs->miirx_data);
  regs->miiaddress = 0;
  regs->miitx_data = 0;
  regs->miicommand = OETH_MIICOMMAND_SCANSTAT;
  printk("Oeth: regs->miiaddress %x regs->miicommand %x\n",regs->miiaddress, regs->miicommand);  
  //regs->miicommand = 0; 
  volatile int i; for(i=0;i<1000;i++);
   while(regs->miistatus & OETH_MIISTATUS_BUSY) ;
   //spin_cursor(); 
   //printk("\r"); 
   //or32_exit(0);
}

void 
eth_mii_write(char phynum, short regnum, short data)
{
  static volatile oeth_regs *regs = (oeth_regs *)(OETH_REG_BASE);
  regs->miiaddress = (regnum << 8) | phynum;
  regs->miitx_data = data;
  regs->miicommand = OETH_MIICOMMAND_WCTRLDATA;
  regs->miicommand = 0; 
  while(regs->miistatus & OETH_MIISTATUS_BUSY);
}

short 
eth_mii_read(char phynum, short regnum)
{
  static volatile oeth_regs *regs = (oeth_regs *)(OETH_REG_BASE);
  regs->miiaddress = (regnum << 8) | phynum;
  regs->miicommand = OETH_MIICOMMAND_RSTAT;
  regs->miicommand = 0; 
  while(regs->miistatus & OETH_MIISTATUS_BUSY);
  
  return regs->miirx_data;
}
	  
void ethphy_init(void)
{

  /* Init the Alaska 88E1111 Phy */
  char alaska88e1111_ml501_phynum = 0x7;

  /* Init, reset */
  short ctl = eth_mii_read(alaska88e1111_ml501_phynum, MII_BMCR);
  ctl &= ~(BMCR_FULLDPLX|BMCR_SPEED100|BMCR_SPD2|BMCR_ANENABLE);
  ctl |= BMCR_SPEED100; // 100MBit
  ctl |= BMCR_FULLDPLX; // Full duplex
  eth_mii_write(alaska88e1111_ml501_phynum, MII_BMCR, ctl);

  // Setup Autoneg
  short adv = eth_mii_read(alaska88e1111_ml501_phynum, MII_ADVERTISE);
  adv &= ~(ADVERTISE_ALL | ADVERTISE_100BASE4 | ADVERTISE_1000XFULL
	   |ADVERTISE_1000XHALF | ADVERTISE_1000XPAUSE |
	   ADVERTISE_1000XPSE_ASYM);
  adv |= ADVERTISE_10HALF;
  adv |= ADVERTISE_10FULL;
  adv |= ADVERTISE_100HALF;
  adv |= ADVERTISE_100FULL;
  eth_mii_write(alaska88e1111_ml501_phynum, MII_ADVERTISE, adv);
  // Disable gigabit???
  adv = eth_mii_read(alaska88e1111_ml501_phynum, MII_M1011_PHY_SPEC_CONTROL);
  adv &= ~(MII_1000BASETCONTROL_FULLDUPLEXCAP |
	   MII_1000BASETCONTROL_HALFDUPLEXCAP);
  eth_mii_write(alaska88e1111_ml501_phynum, MII_M1011_PHY_SPEC_CONTROL, adv);
  // Even more disable gigabit?!
  adv = eth_mii_read(alaska88e1111_ml501_phynum, MII_CTRL1000);
  adv &= ~(ADVERTISE_1000FULL | ADVERTISE_1000HALF);
  eth_mii_write(alaska88e1111_ml501_phynum, MII_CTRL1000, adv);

  // Restart autoneg
  printk("Resetting phy...\n");  
  ctl = eth_mii_read(alaska88e1111_ml501_phynum, MII_BMCR);
  ctl |= (BMCR_ANENABLE | BMCR_ANRESTART);
  eth_mii_write(alaska88e1111_ml501_phynum, MII_BMCR, ctl);

  
}


void ethmac_setup(void)
{
  // from arch/or32/drivers/open_eth.c
  volatile oeth_regs *regs;
  
  regs = (oeth_regs *)(OETH_REG_BASE);
  
  /* Reset MII mode module */
  regs->miimoder = OETH_MIIMODER_RST; /* MII Reset ON */
  regs->miimoder &= ~OETH_MIIMODER_RST; /* MII Reset OFF */
  regs->miimoder = 0x64; /* Clock divider for MII Management interface */
  
  /* Reset the controller.
   */
  regs->moder = OETH_MODER_RST;	/* Reset ON */
  regs->moder &= ~OETH_MODER_RST;	/* Reset OFF */
  
  /* Setting TXBD base to OETH_TXBD_NUM.
   */
  regs->tx_bd_num = OETH_TXBD_NUM;
  
  
  /* Set min/max packet length 
   */
  regs->packet_len = 0x00400600;
  
  /* Set IPGT register to recomended value 
   */
  regs->ipgt = 0x12;
  
  /* Set IPGR1 register to recomended value 
   */
  regs->ipgr1 = 0x0000000c;
  
  /* Set IPGR2 register to recomended value 
   */
  regs->ipgr2 = 0x00000012;
  
  /* Set COLLCONF register to recomended value 
   */
  regs->collconf = 0x000f003f;
  
  /* Set control module mode 
   */
#if 0
  regs->ctrlmoder = OETH_CTRLMODER_TXFLOW | OETH_CTRLMODER_RXFLOW;
#else
  regs->ctrlmoder = 0;
#endif
  
  /* Clear MIIM registers */
  regs->miitx_data = 0;
  regs->miiaddress = 0;
  regs->miicommand = 0;
  
  regs->mac_addr1 = ETH_MACADDR0 << 8 | ETH_MACADDR1;
  regs->mac_addr0 = ETH_MACADDR2 << 24 | ETH_MACADDR3 << 16 | ETH_MACADDR4 << 8 | ETH_MACADDR5;
  
  /* Clear all pending interrupts 
   */
  regs->int_src = 0xffffffff;
  
  /* Promisc, IFG, CRCEn
   */
  regs->moder |= OETH_MODER_PRO | OETH_MODER_PAD | OETH_MODER_IFG | OETH_MODER_CRCEN | OETH_MODER_FULLD;
  
  /* Enable interrupt sources.
   */

  regs->int_mask = OETH_INT_MASK_TXB 	| 
    OETH_INT_MASK_TXE 	| 
    OETH_INT_MASK_RXF 	| 
    OETH_INT_MASK_RXE 	|
    OETH_INT_MASK_BUSY 	|
    OETH_INT_MASK_TXC	|
    OETH_INT_MASK_RXC;

  // Buffer setup stuff
  volatile oeth_bd *tx_bd, *rx_bd;
  int i,j,k;
  
  /* Initialize TXBD pointer
   */
  tx_bd = (volatile oeth_bd *)OETH_BD_BASE;
  
  /* Initialize RXBD pointer
   */
  rx_bd = ((volatile oeth_bd *)OETH_BD_BASE) + OETH_TXBD_NUM;
  
  /* Preallocated ethernet buffer setup */
  unsigned long mem_addr = ETH_BUFF_BASE; /* Defined at top */

  // Setup TX Buffers
  for(i = 0; i < OETH_TXBD_NUM; i++) {
      //tx_bd[i].len_status = OETH_TX_BD_PAD | OETH_TX_BD_CRC | OETH_RX_BD_IRQ;
      tx_bd[i].len_status = OETH_TX_BD_PAD | OETH_TX_BD_CRC;
      tx_bd[i].addr = mem_addr;
      mem_addr += OETH_TX_BUFF_SIZE;
  }
  tx_bd[OETH_TXBD_NUM - 1].len_status |= OETH_TX_BD_WRAP;

  // Setup RX buffers
  for(i = 0; i < OETH_RXBD_NUM; i++) {
    rx_bd[i].len_status = OETH_RX_BD_EMPTY | OETH_RX_BD_IRQ; // Init. with IRQ
    rx_bd[i].addr = mem_addr;
    mem_addr += OETH_RX_BUFF_SIZE;
  }
  rx_bd[OETH_RXBD_NUM - 1].len_status |= OETH_RX_BD_WRAP; // Last buffer wraps

  /* Enable just the receiver
   */
  regs->moder &= ~(OETH_MODER_RXEN | OETH_MODER_TXEN);
  regs->moder |= OETH_MODER_RXEN /* | OETH_MODER_TXEN*/;

  return;
}


/* The interrupt handler.
 */
void
oeth_interrupt(void)
{

  volatile oeth_regs *regs;
  regs = (oeth_regs *)(OETH_REG_BASE);

  uint	int_events;
  int serviced;
  
	serviced = 0;

	/* Get the interrupt events that caused us to be here.
	 */
	int_events = regs->int_src;
	regs->int_src = int_events;

	
	/* Handle receive event in its own function.
	 */
	if (int_events & (OETH_INT_RXF | OETH_INT_RXE)) {
		serviced |= 0x1; 
		oeth_rx();
	}

	/* Handle transmit event in its own function.
	 */
	if (int_events & (OETH_INT_TXB | OETH_INT_TXE)) {
		serviced |= 0x2;
		oeth_tx();
		serviced |= 0x2;
		
	}

	/* Check for receive busy, i.e. packets coming but no place to
	 * put them. 
	 */
	if (int_events & OETH_INT_BUSY) {
		serviced |= 0x4;
		if (!(int_events & (OETH_INT_RXF | OETH_INT_RXE)))
		  oeth_rx();
	}
	
	return;
}



static void
oeth_rx(void)
{
  volatile oeth_regs *regs;
  regs = (oeth_regs *)(OETH_REG_BASE);

  volatile oeth_bd *rx_bdp;
  int	pkt_len, i;
  int	bad = 0;
  
  rx_bdp = ((oeth_bd *)OETH_BD_BASE) + OETH_TXBD_NUM;
  
  printk("r");
  
  /* Find RX buffers marked as having received data */
  for(i = 0; i < OETH_RXBD_NUM; i++)
    {
      bad=0;
      if(!(rx_bdp[i].len_status & OETH_RX_BD_EMPTY)){ /* Looking for NOT empty buffers desc. */
	/* Check status for errors.
	 */
	report(i);
	report(rx_bdp[i].len_status);
	if (rx_bdp[i].len_status & (OETH_RX_BD_TOOLONG | OETH_RX_BD_SHORT)) {
	  bad = 1;
	  report(0xbaad0001);
	}
	if (rx_bdp[i].len_status & OETH_RX_BD_DRIBBLE) {
	  bad = 1;
	  report(0xbaad0002);
	}
	if (rx_bdp[i].len_status & OETH_RX_BD_CRCERR) {
	  bad = 1;
	  report(0xbaad0003);
	}
	if (rx_bdp[i].len_status & OETH_RX_BD_OVERRUN) {
	  bad = 1;
	  report(0xbaad0004);
	}
	if (rx_bdp[i].len_status & OETH_RX_BD_MISS) {
	  report(0xbaad0005);
	}
	if (rx_bdp[i].len_status & OETH_RX_BD_LATECOL) {
	  bad = 1;
	  report(0xbaad0006);
	}
	if (bad) {
	  rx_bdp[i].len_status &= ~OETH_RX_BD_STATS;
	  rx_bdp[i].len_status |= OETH_RX_BD_EMPTY;
	  //exit(0xbaaaaaad);
	  
	  continue;
	}
	else {
	  /* 
	  Process the incoming frame.
	   */
	  pkt_len = rx_bdp[i].len_status >> 16;
	  
	  /* finish up */
	  rx_bdp[i].len_status &= ~OETH_RX_BD_STATS; /* Clear stats */
	  rx_bdp[i].len_status |= OETH_RX_BD_EMPTY; /* Mark RX BD as empty */
	  rx_done++;	
	  report(rx_done);
	}	
      }
    }
}



static void
oeth_tx(void)
{
  volatile oeth_bd *tx_bd;
  int i;
  
  tx_bd = (volatile oeth_bd *)OETH_BD_BASE; /* Search from beginning*/
  
  /* Go through the TX buffs, search for one that was just sent */
  for(i = 0; i < OETH_TXBD_NUM; i++)
    {
      /* Looking for buffer NOT ready for transmit. and IRQ enabled */
      if( (!(tx_bd[i].len_status & (OETH_TX_BD_READY))) && (tx_bd[i].len_status & (OETH_TX_BD_IRQ)) )
	{
	  /* Single threaded so no chance we have detected a buffer that has had its IRQ bit set but not its BD_READ flag. Maybe this won't work in linux */
	  tx_bd[i].len_status &= ~OETH_TX_BD_IRQ;

	  /* Probably good to check for TX errors here */
	  
	  /* set our test variable */
	  tx_done = 1;

	  printk("T%d",i);
	  
	}
    }
  return;  
}

// Loop to check if a number is prime by doing mod divide of the number
// to test by every number less than it
int 
is_prime_number(unsigned long n)
{
  unsigned long c;
  if (n < 2) return 0;
  for(c=2;c<n;c++)
    if ((n % c) == 0) 
      return 0;
  return 1;
}

int main ()
{
  
  /* Initialise handler vector */
  int_init();

  /* Install ethernet interrupt handler, it is enabled here too */
  int_add(ETH0_IRQ, oeth_interrupt, 0);

  /* Enable interrupts in supervisor register */
  mtspr (SPR_SR, mfspr (SPR_SR) | SPR_SR_IEE);
  
  rx_done = 0;
  
  ethmac_setup(); /* Configure MAC, TX/RX BDs and enable RX in MODER */

#define NUM_PRIMES_TO_CHECK 1000

  char prime_check_results[NUM_PRIMES_TO_CHECK];
  unsigned long num_to_check;

      for(num_to_check=2;num_to_check<NUM_PRIMES_TO_CHECK;num_to_check++)
	{
	  prime_check_results[num_to_check-2] 
	    = (char) is_prime_number(num_to_check);
	  report(num_to_check | (0x1e<<24));
	  report(prime_check_results[num_to_check-2] | (0x2e<<24));
	  if (rx_done >= 255) // Check number of packets received, testbench
	                     // will hopefully send at least 256 packets
	    exit(0x8000000d);
	}
      exit(0xbaaaaaad);
}
