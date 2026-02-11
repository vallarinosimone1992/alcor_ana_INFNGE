#ifndef __ALCOR_ALCOR_H__
#define __ALCOR_ALCOR_H__

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

#define AINC               0x8000
#define BCR(reg)         ( 0x1000 | reg )
#define ECCR(reg)        ( 0x2000 | reg )
#define PCR(reg,pix,col) ( 0x4000 | reg | (pix << 2) | (col << 5) )

namespace alcor {

  struct eccr_t {
    uint16_t Column0_Enable    : 1;
    uint16_t Column0_SafeBit   : 1;
    uint16_t Column0_Iratio    : 1;
    uint16_t Column1_Enable    : 1;
    uint16_t Column1_SafeBit   : 1;
    uint16_t Column1_Iratio    : 1;
    uint16_t not_used          : 5;
    uint16_t Raw_Mode          : 1;
    uint16_t Encoder_Enable    : 1;
    uint16_t Serializer_Enable : 1;
    uint16_t Serializer_Align  : 1;
    uint16_t Status_Enable     : 1;
  };

  struct bcr0246_t {
    uint16_t iblatchDAC : 2;
    uint16_t iTDC       : 5;
    uint16_t cal        : 3;
    uint16_t not_used   : 6;
    void print() {
      std::cout << "iblatchDAC = " << iblatchDAC << " | "
		<< "iTDC = " << iTDC << " | "
		<< "cal = " << cal << std::endl;
    };
  };

  struct bcr1357_t {
    uint16_t Bit_cg    : 5;
    uint16_t Bit_boost : 5;
    uint16_t S0        : 1;
    uint16_t ib_sF     : 1;
    uint16_t ib_3      : 2;
    uint16_t ib_2      : 2;
    void print() {
      std::cout << "Bit_cg = " << Bit_cg << " | "
		<< "Bit_boost = " << Bit_boost << " | "
		<< "S0 = " << S0 << " | "
		<< "ib_sF = " << ib_sF << " | "
		<< "ib_3 = " << ib_3 << " | "
		<< "ib_2 = " << ib_2 << std::endl;
    };
  };

  struct pcr0_t {
    uint16_t cDAC_TDC0 : 4;
    uint16_t cDAC_TDC1 : 4;
    uint16_t cDAC_TDC2 : 4;
    uint16_t cDAC_TDC3 : 4;
    /*
    pcr0_t() : 
      cDAC_TDC0(0x7),
      cDAC_TDC1(0x7),
      cDAC_TDC2(0x7),
      cDAC_TDC3(0x7) {};
    int value() { return *((int *)(this)); };
    */
  };

  struct pcr1_t {
    uint16_t fDAC_TDC0 : 4;
    uint16_t fDAC_TDC1 : 4;
    uint16_t fDAC_TDC2 : 4;
    uint16_t fDAC_TDC3 : 4;
    /*
    pcr1_t() :
      fDAC_TDC0(0x8),
      fDAC_TDC1(0x8),
      fDAC_TDC2(0x8),
      fDAC_TDC3(0x8) {};
    int value() { return *((int *)(this)); };
    */
  };

  struct pcr2_t {
    uint16_t LE1DAC     : 6;
    uint16_t LEDACrange : 2;
    uint16_t LEDACVth   : 2;
    uint16_t LE2DAC     : 6;
    /*
    pcr2_t() :
      LE1DAC(0x3f),
      LEDACrange(0x3),
      LEDACVth(0x3),
      LE2DAC(0x3f) {};
    int value() { return *((int *)(this)); };
    */
    void print() {
      std::cout << "LE1DAC = " << LE1DAC << " | "
		<< "LEDACrange = " << LEDACrange << " | "
		<< "LEDACVth = " << LEDACVth << " | "
		<< "LE2DAC = " << LE2DAC << std::endl;
    };
  };

  struct pcr3_t {
    uint16_t not_used : 1;
    uint16_t Polarity : 1;
    uint16_t Gain2    : 2;
    uint16_t Gain1    : 2;
    uint16_t Offset2  : 3;
    uint16_t OpMode   : 4;
    uint16_t Offset1  : 3;
    /*
    pcr3_t() :
      not_used(0x0),
      Polarity(0x1),
      Gain2(0x0),
      Gain1(0x0),
      Offset2(0x0),
      OpMode(0x0),
      Offset1(0x0) {};
    int value() { return *((int *)(this)); };
    */
    void print() {
      std::cout << "Polarity = " << Polarity << " | "
		<< "Gain2 = " << Gain2 << " | "
		<< "Gain1 = " << Gain1 << " | "
		<< "Offset2 = " << Offset2 << " | "
		<< "OpMode = " << OpMode << " | "
		<< "Offset1 = " << Offset1 << std::endl;
    };
  };


  typedef union bcr0246_union_t {
    uint16_t val;
    struct bcr0246_t reg;
  } bcr0246_union_t;
  
  typedef union bcr1357_union_t {
    uint16_t val;
    struct bcr1357_t reg;
  } bcr1357_union_t;
  
  typedef union pcr2_union_t {
    uint16_t val;
    struct pcr2_t reg;
  } pcr2_union_t;
  
  typedef union pcr3_union_t {
    uint16_t val;
    struct pcr3_t reg;
  } pcr3_union_t;


  class spi_t {

  public:
    uhal::HwInterface *hardware;
    const uhal::Node *tx0_rx0;
    const uhal::Node *tx1_rx1;
    const uhal::Node *tx2_rx2;
    const uhal::Node *tx3_rx3;
    const uhal::Node *ctrl;
    const uhal::Node *divider;
    const uhal::Node *ss;

    spi_t() = default;

    void init(uhal::HwInterface &hw, int chip) {
      hardware = &hw;
      tx0_rx0  = &hw.getNode("spi_id" + std::to_string(chip) + ".tx0_rx0");
      tx1_rx1  = &hw.getNode("spi_id" + std::to_string(chip) + ".tx1_rx1");
      tx2_rx2  = &hw.getNode("spi_id" + std::to_string(chip) + ".tx2_rx2");
      tx3_rx3  = &hw.getNode("spi_id" + std::to_string(chip) + ".tx3_rx3");
      ctrl     = &hw.getNode("spi_id" + std::to_string(chip) + ".ctrl");
      divider  = &hw.getNode("spi_id" + std::to_string(chip) + ".divider");
      ss       = &hw.getNode("spi_id" + std::to_string(chip) + ".ss");
    };

    int cmd(int command, int tdata) {
      int data;
      //      divider->write(0xf);
      divider->write(0x1f); // good for 4m cables
      ss->write(0x1);
      
      data = bS(CTRL_ASS) | bS(CTRL_RX_NEGEDGE) | 0x18;
      ctrl->write(data);
      
      data = CMD(command) | (tdata & 0xffff);
      tx0_rx0->write(data);
      
      data = bS(CTRL_ASS) | bS(CTRL_RX_NEGEDGE) | bS(CTRL_GO) | 0x18;
      ctrl->write(data);
      
      data = bS(CTRL_ASS) | bS(CTRL_RX_NEGEDGE) | 0x18;
      ctrl->write(data);
      
      hardware->dispatch();
      
      if (command != RDAT) return 0;

      uhal::ValWord<uint32_t> reg = tx0_rx0->read();
      hardware->dispatch();
      return (reg.value() & 0xffff);
    };
    
    int cmd_hack(int command, int tdata) {
      int data;
      //      divider->write(0xf);
      divider->write(0x1f); // good for 4m cables
      ss->write(0x1);
      
      data = bS(CTRL_ASS) | bS(CTRL_RX_NEGEDGE) | 0x18;
      ctrl->write(data);
      
      data = CMD(command) | (tdata & 0xffff);
      tx0_rx0->write(data);
      
      data = bS(CTRL_ASS) | bS(CTRL_RX_NEGEDGE) | bS(CTRL_GO) | 0x18;
      ctrl->write(data);
      
      data = bS(CTRL_ASS) | bS(CTRL_RX_NEGEDGE) | 0x18;
      ctrl->write(data);
      
      //      hardware->dispatch();
      
      if (command != RDAT) return 0;

      uhal::ValWord<uint32_t> reg = tx0_rx0->read();
      hardware->dispatch();
      return (reg.value() & 0xffff);
    };
    
    int read(int address) {
      cmd(WPTR, address);
      return cmd(RDAT, 0);
    };
    
    int write(int address, int data) {
      cmd(WPTR, address);
      cmd(WDAT, data);
      return 0; //cmd(RDAT, 0);
    };
    
    int read_block(int address, int n, int *data) {
      cmd(WPTR, address | AINC);
      for (int i = 0; i < n; ++i)
	data[i] = cmd(RDAT, 0);
      return 0;
    };
    
    int write_block(int address, int n, int *data) {
      cmd(WPTR, address | AINC);
      for (int i = 0; i < n; ++i)
	cmd(WDAT, data[i]);
      return 0; //cmd(RDAT, 0);
    };
    
  };

  class fifo_t {
  public:
    const uhal::Node *occupancy;
    const uhal::Node *reset;
    const uhal::Node *data;
    const uhal::Node *timer;
    
    fifo_t() = default;

    void init(uhal::HwInterface &hw, int chip, int lane) {
      occupancy = &hw.getNode("alcor_readout_id" + std::to_string(chip) + "_lane" + std::to_string(lane) + ".fifo_occupancy");
      reset = &hw.getNode("alcor_readout_id" + std::to_string(chip) + "_lane" + std::to_string(lane) + ".fifo_reset");
      data = &hw.getNode("alcor_readout_id" + std::to_string(chip) + "_lane" + std::to_string(lane) + ".fifo_data");
      timer = &hw.getNode("alcor_readout_id" + std::to_string(chip) + "_lane" + std::to_string(lane) + ".fifo_timer");
    };
  };

  class service_t {
  public:
    const uhal::Node *reset, *testpulse, *controller;
    service_t() = default;
    void init(uhal::HwInterface &hw, int chip) {
      reset = &hw.getNode("pulser.reset_id" + std::to_string(chip));
      testpulse = &hw.getNode("pulser.testpulse_id" + std::to_string(chip));
      controller = &hw.getNode("alcor_controller_id" + std::to_string(chip));
    }
  };

  class alcor_t {
  public:
    uhal::HwInterface *hardware;
    service_t service;
    spi_t spi;
    fifo_t fifo[4];
    alcor_t() = default;
    void init(uhal::HwInterface &hw, int c) {
      hardware = &hw;
      service.init(hw, c);
      spi.init(hw, c);
      for (int lane = 0; lane < 4; ++lane)
	fifo[lane].init(hw, c, lane);
    };
    void reset(bool now = true) {
      service.reset->write(0x1);
      if (now) hardware->dispatch(); 
    };
    void pulse(bool now = true) {
      service.testpulse->write(0x1);
      if (now) hardware->dispatch(); 
    };
    void hard_reset(bool now = true) {
      write_controller(0x500001, now);
    };
    void write_controller(int data, bool now = true) {
      service.controller->write(data);
      if (now) hardware->dispatch(); 
    }
    bool load_sequence(std::string filename) {
      return true;
    }
    bool initialise() {
      union eccr_union_t {
	struct eccr_t reg;
	uint16_t val;
      } eccr;
      eccr.val = 0;

      eccr.reg.Serializer_Align = 1;
      for (int i = 0; i < 4; ++i)
	spi.write(ECCR(i), eccr.val);
      hardware->dispatch();
      if (!load_sequence("alcor-sync.cfg"))
	return false;

      eccr.reg.Serializer_Align = 0;
      for (int i = 0; i < 4; ++i)
	spi.write(ECCR(i), eccr.val);
      hardware->dispatch();
      if (!load_sequence("alcor-sync.cfg"))
	return false;

      return true;
    }
  };

  class regfile_t {
  public:
    const uhal::Node *fwrev, *board_id, *status, *ctrl, *debug, *mode;
    regfile_t() = default;
    void init(uhal::HwInterface &hw) {
      fwrev = &hw.getNode("regfile.fwrev");
      board_id = &hw.getNode("regfile.board_id");
      status = &hw.getNode("regfile.status");
      ctrl = &hw.getNode("regfile.ctrl");
      debug = &hw.getNode("regfile.debug");
      mode = &hw.getNode("regfile.mode");
    }
  };

  class trigger_t {
  public:
    const uhal::Node *occupancy;
    const uhal::Node *reset;
    const uhal::Node *data;
    
    trigger_t() = default;

    void init(uhal::HwInterface &hw) {
      occupancy = &hw.getNode("trigger_info.fifo_occupancy");
      reset = &hw.getNode("trigger_info.fifo_reset");
      data = &hw.getNode("trigger_info.fifo_data");
    };
  };

  class daq_t {
  public:
    daq_t() = default;
    uhal::HwInterface *hardware;
    regfile_t regfile;
    trigger_t trigger;
    alcor_t alcor[6];
    void init(uhal::HwInterface &hw) {
      regfile.init(hw);
      trigger.init(hw);
      for (int chip = 0; chip < 6; ++chip)
	alcor[chip].init(hw, chip);
    };
  };

}

#endif /** __ALCOR_ALCOR_H__ **/
