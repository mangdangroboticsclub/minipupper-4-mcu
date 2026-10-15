# Host test of the robot's policy arithmetic against omniRL's numpy.
#   make OMNIRL=../../../../omniRL POLICY=bam_t5ub06_stops_v1
OMNIRL ?= ../../../../omniRL
POLICY ?= bam_t5ub06_stops_v1
SRC = ../../../components/mpx_policy/src
MP = $(OMNIRL)/mp3_walk_policy

all: test
host_policy: host_policy.c $(SRC)/policy_core.c $(SRC)/policy_core.h
	$(CC) -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE -I$(SRC) host_policy.c $(SRC)/policy_core.c -lm -o $@
test.mpxp:
	python3 ../export_mpxp.py $(OMNIRL)/policies/$(POLICY) --contract $(MP)/data/sku2_contract.json \
	    --mount $(MP)/data/imu_mount.json --robot sku2 --out $@
test: host_policy test.mpxp
	python3 compare.py $(MP) $(OMNIRL)/policies/$(POLICY) test.mpxp ./host_policy
clean:
	rm -f host_policy test.mpxp
.PHONY: all test clean
