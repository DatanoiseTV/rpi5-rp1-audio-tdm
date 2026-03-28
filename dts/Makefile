DTC ?= dtc
DTC_FLAGS ?= -@ -I dts -O dtb -W no-unit_address_vs_reg

OVERLAYS := rp1-audio-tdm.dtbo

all: $(OVERLAYS)

%.dtbo: %.dts
	$(DTC) $(DTC_FLAGS) -o $@ $<

clean:
	rm -f *.dtbo

.PHONY: all clean
