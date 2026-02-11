#ifndef __ALCOR_SPI_H__
#define __ALCOR_SPI_H__

#include "uhal/uhal.hpp"

#define CTRL_BIT_NB    	14
#define CTRL_ASS       	13
#define CTRL_IE	       	12 
#define CTRL_LSB	11 
#define CTRL_TX_NEGEDGE 10 
#define CTRL_RX_NEGEDGE 9 
#define CTRL_GO 	8 
#define CTRL_RES_1 	7
#define CTRL_CHAR_LEN   6 
#define COMMAND         20
#define POINTER         12
#define RPTR            8
#define WPTR            0
#define RDAT            9
#define WDAT            1
#define RSREG           8

#define CMD(x) ( x << COMMAND )
#define bS(x)  ( 1 << x )

#define BCR(reg)         ( 0x1000 | reg )
#define ECCR(reg)        ( 0x2000 | reg )
#define PCR(reg,pix,col) ( 0x4000 | reg | (pix << 2) | (col << 5) )

struct spi_t {
  uhal::HwInterface *hardware;
  const uhal::Node *tx0_rx0;
  const uhal::Node *tx1_rx1;
  const uhal::Node *tx2_rx2;
  const uhal::Node *tx3_rx3;
  const uhal::Node *ctrl;
  const uhal::Node *divider;
  const uhal::Node *ss;
  void populate(uhal::HwInterface &hw, int chip) {
    hardware = &hw;
    tx0_rx0  = &hw.getNode("spi_id" + std::to_string(chip) + ".tx0_rx0");
    tx1_rx1  = &hw.getNode("spi_id" + std::to_string(chip) + ".tx1_rx1");
    tx2_rx2  = &hw.getNode("spi_id" + std::to_string(chip) + ".tx2_rx2");
    tx3_rx3  = &hw.getNode("spi_id" + std::to_string(chip) + ".tx3_rx3");
    ctrl     = &hw.getNode("spi_id" + std::to_string(chip) + ".ctrl");
    divider  = &hw.getNode("spi_id" + std::to_string(chip) + ".divider");
    ss       = &hw.getNode("spi_id" + std::to_string(chip) + ".ss");
  }
};

int exec_cmd(spi_t &spi, int command, int tdata);
int alcor_read(spi_t &spi, int address);
int alcor_write(spi_t &spi, int address, int data);


int
exec_cmd(spi_t &spi, int command, int tdata)
{
  int data;
  spi.divider->write(0xf);
  spi.ss->write(0x1);

  data = bS(CTRL_ASS) | bS(CTRL_RX_NEGEDGE) | 0x18;
  spi.ctrl->write(data);

  data = CMD(command) | (tdata & 0xffff);
  spi.tx0_rx0->write(data);

  data = bS(CTRL_ASS) | bS(CTRL_RX_NEGEDGE) | bS(CTRL_GO) | 0x18;
  spi.ctrl->write(data);
  
  data = bS(CTRL_ASS) | bS(CTRL_RX_NEGEDGE) | 0x18;
  spi.ctrl->write(data);
  
  spi.hardware->dispatch();

  uhal::ValWord<uint32_t> reg = spi.tx0_rx0->read();
  spi.hardware->dispatch();
  return (reg.value() & 0xffff);
}

int alcor_read(spi_t &spi, int address)
{
  exec_cmd(spi, WPTR, address);
  return exec_cmd(spi, RDAT, 0);
}

int alcor_write(spi_t &spi, int address, int data)
{
  exec_cmd(spi, WPTR, address);
  exec_cmd(spi, WDAT, data);
  return exec_cmd(spi, RDAT, 0);
}

#endif /** __ALCOR_SPI_H__ **/
