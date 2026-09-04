		/* C68 profile only: second HP hart. Not the LP core. */
		CPU1: cpu@1 {
			device_type = "cpu";
			compatible = "riscv";
			reg = <1>;
			riscv,isa = "rv32imafc";
			mmu-type = "riscv,none";
			status = "okay";

			CPU1_intc: interrupt-controller {
				compatible = "riscv,cpu-intc";
				#interrupt-cells = <1>;
				interrupt-controller;
			};
		};
