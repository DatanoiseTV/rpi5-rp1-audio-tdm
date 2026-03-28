# Kernel build rules for rp1-audio-tdm module
GIT_BRANCH := $(shell cd $(src) && git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")
GIT_HASH := $(shell cd $(src) && git rev-parse --short HEAD 2>/dev/null || echo "unknown")

ccflags-y += -DGIT_BRANCH=\"$(GIT_BRANCH)\" -DGIT_HASH=\"$(GIT_HASH)\"
ccflags-y += -I$(src)/src

obj-m += snd-soc-rp1-audio-tdm.o
snd-soc-rp1-audio-tdm-objs := src/rp1_audio_tdm.o
