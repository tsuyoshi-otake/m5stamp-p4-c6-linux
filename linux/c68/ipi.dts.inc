		/* C68-only HP-core IPI provider. */
		esp32p4_ipi: interrupt-controller@500e5000 {
			compatible = "espressif,esp32p4-ipi";
			reg = <0x500e5000 0x20>;
			#interrupt-cells = <0>;
			interrupt-controller;
			interrupts-extended = <&clic0 16 79 IRQ_TYPE_LEVEL_HIGH>,
					      <&clic0 20 80 IRQ_TYPE_LEVEL_HIGH>;
			status = "okay";
		};
