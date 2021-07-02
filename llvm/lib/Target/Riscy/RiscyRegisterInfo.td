class RiscyReg<bits<5> i, string n> : Register<n> {
	field bits<5> Num = i;
	let Namespace = "Riscy";
}

foreach i = {0 ... 31} in {
	def R#i : RiscyReg<i, "r"#i>, DwarfRegNum<[i]>;
}

def RiscyRegs : RegisterClass<"Riscy", [i32], 32,
	(add R8, R9, R10, R11, R12, R13, R14, R15, R1, R2, R3, R4, R5, R6, R7,
	R24, R25, R26, R27, R28, R29, R30, R31, R16, R17, R18, R20, R21, R22, R23,
	R19, R0)>;
