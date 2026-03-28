# Kernel build rules for rp1-audio-tdm
GIT_BRANCH := $(shell cd $(src) && git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")
GIT_HASH := $(shell cd $(src) && git rev-parse --short HEAD 2>/dev/null || echo "unknown")

ccflags-y += -DGIT_BRANCH=\"$(GIT_BRANCH)\" -DGIT_HASH=\"$(GIT_HASH)\"
ccflags-y += -I$(src)/src

# Machine driver (links DWC I2S + PIO I2S)
obj-m += snd-soc-rp1-audio-tdm.o
snd-soc-rp1-audio-tdm-objs := src/rp1_audio_tdm.o

# PIO I2S expansion (additional data lanes via PIO state machines)
obj-m += snd-soc-rp1-pio-i2s.o
snd-soc-rp1-pio-i2s-objs := src/rp1_pio_i2s.o
