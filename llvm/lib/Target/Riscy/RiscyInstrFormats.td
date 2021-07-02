class InstRiscy<dag outs, dag ins, string asmstr, list<dag> pattern>
	: Instruction {

	field bits<32> SoftFail = 0;
	field bits<32> Inst;
	let Size = 4;

	let Namespace = "Riscy";

	bits<6> Opcode;
	let Inst{31-26} = Opcode;

	dag OutOperandList = outs;
	dag InOperandList = ins;
	let AsmString = asmstr;
	let Pattern = pattern;

	let DecoderNamespace = "Riscy";
}

class InstMem<bits<6> op, dag outs, dag ins, string asmstr, list<dag> pattern>
	: InstRiscy<outs, ins, asmstr, pattern> {

	let Opcode = op;
	bits<22> memory;
	bits<4> register;
	let Inst{25-4} = memory;
	let Inst{3-0} = register;
}

class InstReg<bits<7> op, dag outs, dag ins, string asmstr, list<dag> pattern>
	: InstRiscy<outs, ins, asmstr, pattern> {

	let Opcode = op{5-0};
	let Inst{25} = op{6};
	bits<5> registerA;
	bit vA;
	bit cA;
	bits<5> registerB;
	bit vB;
	bit cB;
	bits<5> registerC;
	bits<5> registerD;
	bit vC;
	let Inst{0-4} = registerA;
	let Inst{5-9} = registerB;
	let Inst{10-14} = registerC;
	let Inst{15-19} = registerD;
	let Inst{20} = vA;
	let Inst{21} = vB;
	let Inst{22} = cA;
	let Inst{23} = cB;
	let Inst{24} = vC;
}
