#include "vt168.hpp"
#include "6502/mos6502.hpp"
#include "dma.hpp"
#include "extalu.hpp"
#include "input.hpp"
#include "irq.hpp"
#include "miwi2_input.hpp"
#include "mmu.hpp"
#include "ppu.hpp"
#include "scpu_mem.hpp"
#include "timer.hpp"
#include "util.hpp"

#include <cassert>
#include <chrono>
#include <string>
#include <vector>
using namespace std;

namespace VTxx {

static mos6502::mos6502 *cpu, *scpu;
static ExtALU *cpu_alu, *scpu_alu;
static Timer *cpu_timer, *scpu_timer0, *scpu_timer1;
static IRQController *cpu_irq, *scpu_irq;

static DMACtrl *cpu_dma;

static InputDev *inp;
static MiWi2Input *mw2inp = nullptr;
static const vector<IRQVector> cpu_vectors = {
    {0xFFFF, 0xFFFE}, // 0 EXT
    {0xFFF9, 0xFFF8}, // 1 TIMER
    {0xFFF7, 0xFFF6}, // 2 SPU
    {0xFFF5, 0xFFF4}, // 3 UART
    {0xFFF3, 0xFFF2}, // 4 SPI
};

static const vector<IRQVector> scpu_vectors = {
    {0x0FFF, 0x0FFE}, // 0 EXT
    {0x0FF9, 0x0FF8}, // 1 TIMERA
    {0x0FF7, 0x0FF6}, // 2 TIMERB
    {0x0FF5, 0x0FF4}  // 3 CPU
};
static int fcount = 0;
static int last_fcount = 0;

chrono::system_clock::time_point last_update;

void vt168_init(VT168_Platform plat, const std::string &rom) {
  mmu_init();
  ppu_init();
  if (rom != "")
    load_rom(rom);

  cpu = new mos6502::mos6502(read_mem_virtual, write_mem_virtual);
  if (plat == VT168_Platform::VT168_MIWI2)
    cpu->scramble = true;

  scpu = new mos6502::mos6502(scpu_read_mem, scpu_write_mem);
  scpu->brkVectorH = 0x0FFF;
  scpu->brkVectorL = 0x0FFE;
  scpu->rstVectorH = 0x0FFD;
  scpu->rstVectorL = 0x0FFC;
  scpu->nmiVectorH = 0x0FFB;
  scpu->nmiVectorL = 0x0FFA;

  cpu_irq = new IRQController(cpu_vectors, cpu);
  reg_read_fn[0x21] = [](uint16_t a) { return cpu_irq->read(0); };
  reg_write_fn[0x21] = [](uint16_t a, uint8_t x) { cpu_irq->write(0, x); };

  scpu_irq = new IRQController(scpu_vectors, scpu);
  scpu_irq->write(0, 0x0F); // no general mask register, set all enabled

  cpu_alu = new ExtALU(true, false);
  scpu_alu = new ExtALU(true, false);
  for (uint8_t a = 0x30; a <= 0x37; a++) {
    reg_read_fn[a] = [](uint16_t a) { return cpu_alu->read(a & 0x0F); };
    scpu_reg_read_fn[a] = [](uint16_t a) { return scpu_alu->read(a & 0x0F); };
    reg_write_fn[a] = [](uint16_t a, uint8_t d) {
      cpu_alu->write(a & 0x0F, d);
    };
    scpu_reg_write_fn[a] = [](uint16_t a, uint8_t d) {
      scpu_alu->write(a & 0x0F, d);
    };
  }

  cpu_timer = new Timer(TimerType::TIMER_VT_CPU,
                        [](bool x) { cpu_irq->set_irq(1, x); });
  scpu_timer0 = new Timer(TimerType::TIMER_VT_SCPU,
                          [](bool x) { scpu_irq->set_irq(1, x); });
  scpu_timer1 = new Timer(TimerType::TIMER_VT_SCPU,
                          [](bool x) { scpu_irq->set_irq(2, x); });
  for (int i = 0; i < 4; i++) {
    reg_read_fn[0x01 + i] = [](uint16_t a) {
      return cpu_timer->read(a - 0x2101);
    };
    reg_write_fn[0x01 + i] = [](uint16_t a, uint8_t b) {
      cpu_timer->write(a - 0x2101, b);
    };
    scpu_reg_read_fn[0x0 + i] = [](uint16_t a) {
      return scpu_timer0->read(a & 0x03);
    };
    scpu_reg_read_fn[0x10 + i] = [](uint16_t a) {
      return scpu_timer1->read(a & 0x03);
    };
    scpu_reg_write_fn[0x0 + i] = [](uint16_t a, uint8_t b) {
      scpu_timer0->write(a & 0x03, b);
    };
    scpu_reg_write_fn[0x10 + i] = [](uint16_t a, uint8_t b) {
      scpu_timer1->write(a & 0x03, b);
    };
  }
  reg_read_fn[0x0B] = [](uint16_t a) { return cpu_timer->read(0xA); };
  reg_write_fn[0x0B] = [](uint16_t a, uint8_t b) {
    cpu_timer->write(0xA, b);
    control_reg[0x0B] = b;
  };
  cpu_dma = new DMACtrl();
  for (uint8_t a = 0x22; a <= 0x28; a++) {
    reg_read_fn[a] = [](uint16_t a) { return cpu_dma->read(a - 0x2122); };
    reg_write_fn[a] = [](uint16_t a, uint8_t b) {
      cpu_dma->write(a - 0x2122, b);
    };
  }

  inp = new InputDev();

  reg_read_fn[0x29] = [](uint16_t a) { return inp->read(0); };

  if (plat == VT168_Platform::VT168_MIWI2) {
    mw2inp = new MiWi2Input();
    reg_read_fn[0x0E] = [](uint16_t a) { return mw2inp->read(0); };
    reg_read_fn[0x0F] = [](uint16_t a) { return mw2inp->read(1); };
  }

  reg_write_fn[0x1C] = [](uint16_t a, uint8_t b) {
    control_reg[0x1C] = b;
    scpu_irq->set_irq(3, get_bit(b, 4));
  };

  reg_read_fn[0x1C] = [](uint16_t a) {
    return control_reg[0x1C];
    cpu_irq->set_irq(3, false);
  };

  scpu_reg_write_fn[0x1C] = [](uint16_t a, uint8_t b) {
    scpu_control_reg[0x1c] = b;
    cpu_irq->set_irq(2, get_bit(b, 4));
  };

  scpu_reg_read_fn[0x1C] = [](uint16_t a) {
    scpu_irq->set_irq(3, 0);
    return scpu_control_reg[0x1c];
  };

  // TODO: init misc control regs

  cpu->Reset();
  last_update = chrono::system_clock::now();
}

const int reg_sys = 0x06;

static void vt168_scpu_tick() {
  if (!get_bit(control_reg[reg_sys], 5)) {
    scpu->Reset();
  } else if (get_bit(control_reg[reg_sys], 4)) {
    scpu->Run(1);
  }
  scpu_timer0->tick();
  scpu_timer1->tick();
}

static void vt168_cpu_tick() {
  // cout << "PC: " << va_to_str(cpu->GetPC()) << endl;
  if (!cpu_dma->is_busy())
    cpu->Run(1);
  cpu_timer->tick();
}

static int cpu_ratio = 5; // set to 4 for NTSC
static int cpu_div = 0;
static bool last_vblank = false;

bool vt168_tick() {
  vt168_scpu_tick();
  cpu_div++;
  bool is_vblank = false;

  if (cpu_div == cpu_ratio) {
    cpu_div = 0;
    vt168_cpu_tick();
    ppu_tick();

    if (ppu_is_vblank() && !last_vblank) {
      if (mw2inp != nullptr) {
        mw2inp->notify_vblank();
      }
      /*cout << "PC: " << va_to_str(cpu->GetPC()) << endl;
      cout << "mem[PC]: ";
      for (int i = 0; i < 4; i++) {
        int addr = cpu->GetPC() + i;
        if ((addr < 0x2000) || (addr >= 0x4000))
          cout << hex << int(read_mem_virtual(addr)) << " ";
      }
      cout << endl;
      if (cpu->GetPC() <= 0x104)
        assert(false);*/
      fcount++;
      if (ppu_nmi_enabled()) {
        // cout << "-- NMI --" << endl;
        cpu->NMI();
        if (get_bit(scpu_control_reg[0x1C], 1))
          scpu->NMI();
      }
      if (chrono::duration<double>(chrono::system_clock::now() - last_update)
              .count() > 0.5) {
        double fps =
            double(fcount - last_fcount) /
            (chrono::duration<double>(chrono::system_clock::now() - last_update)
                 .count());
        cout << "speed = " << dec << fps << "fps" << endl;
        last_fcount = fcount;
        last_update = chrono::system_clock::now();
      }
      is_vblank = true;
    }
    if (true)
      cpu_dma->vblank_notify();
    last_vblank = ppu_is_vblank();
  }
  return is_vblank;
}

void vt168_process_event(SDL_Event *ev) {
  inp->process_event(ev);
  if (mw2inp != nullptr) {
    mw2inp->process_event(ev);
  }
}

void vt168_reset() {
  mmu_reset();
  ppu_reset();
  cpu_dma->reset();
  scpu->Reset();
  cpu->Reset();
}

}; // namespace VTxx
