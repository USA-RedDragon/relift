# Maxwell lowering table

Written by `RELIFT_BLESS=1 cargo test -p relift-gpu-sm53 --test lowering`
from `src/lower/table.rs`; do not edit by hand. Each Maxwell form is
lowered to exactly one SM86 form, handled as a pseudo-instruction, or
a stop. A form is lowered only with evidence that both behave the same
in every case.

| form | row | to | evidence or reason |
|---|---|---|---|
| AL2P | stop | | no semantic check yet |
| ALD | stop | | no semantic check yet |
| AST | stop | | no semantic check yet |
| ATOM | stop | | no semantic check yet |
| ATOM_CAS | stop | | no semantic check yet |
| ATOMS | stop | | no semantic check yet |
| ATOMS_CAS | stop | | no semantic check yet |
| B2R | stop | | no semantic check yet |
| BAR | stop | | no semantic check yet |
| BFE R | stop | | no semantic check yet |
| BFE I | stop | | no semantic check yet |
| BFE C | stop | | no semantic check yet |
| BFI R | stop | | no semantic check yet |
| BFI I | stop | | no semantic check yet |
| BFI C | stop | | no semantic check yet |
| BFI RC | stop | | no semantic check yet |
| BPT | stop | | no semantic check yet |
| BRA | stop | | no semantic check yet |
| BRK | stop | | no semantic check yet |
| BRX | stop | | no semantic check yet |
| CAL | stop | | no semantic check yet |
| CCTL | stop | | no semantic check yet |
| CCTLL | stop | | no semantic check yet |
| CCTLT | stop | | no semantic check yet |
| CONT | stop | | no semantic check yet |
| CSET | stop | | no semantic check yet |
| CSETP | stop | | no semantic check yet |
| CS2R | stop | | no semantic check yet |
| DADD R | stop | | no semantic check yet |
| DADD I | stop | | no semantic check yet |
| DADD C | stop | | no semantic check yet |
| DEPBAR | stop | | no semantic check yet |
| DFMA R | stop | | no semantic check yet |
| DFMA I | stop | | no semantic check yet |
| DFMA C | stop | | no semantic check yet |
| DFMA RC | stop | | no semantic check yet |
| DMNMX R | stop | | no semantic check yet |
| DMNMX I | stop | | no semantic check yet |
| DMNMX C | stop | | no semantic check yet |
| DMUL R | stop | | no semantic check yet |
| DMUL I | stop | | no semantic check yet |
| DMUL C | stop | | no semantic check yet |
| DSET R | stop | | no semantic check yet |
| DSET I | stop | | no semantic check yet |
| DSET C | stop | | no semantic check yet |
| DSETP R | stop | | no semantic check yet |
| DSETP I | stop | | no semantic check yet |
| DSETP C | stop | | no semantic check yet |
| EXIT | stop | | no semantic check yet |
| F2F R | stop | | no semantic check yet |
| F2F I | stop | | no semantic check yet |
| F2F C | stop | | no semantic check yet |
| F2I R | stop | | no semantic check yet |
| F2I I | stop | | no semantic check yet |
| F2I C | stop | | no semantic check yet |
| FADD R | stop | | no semantic check yet |
| FADD I | stop | | no semantic check yet |
| FADD C | stop | | no semantic check yet |
| FADD 32I | stop | | no semantic check yet |
| FCHK R | stop | | no semantic check yet |
| FCHK I | stop | | no semantic check yet |
| FCHK C | stop | | no semantic check yet |
| FCMP R | stop | | no semantic check yet |
| FCMP I | stop | | no semantic check yet |
| FCMP C | stop | | no semantic check yet |
| FCMP RC | stop | | no semantic check yet |
| FFMA R | stop | | no semantic check yet |
| FFMA I | stop | | no semantic check yet |
| FFMA C | stop | | no semantic check yet |
| FFMA RC | stop | | no semantic check yet |
| FFMA 32I | stop | | no semantic check yet |
| FLO R | stop | | no semantic check yet |
| FLO I | stop | | no semantic check yet |
| FLO C | stop | | no semantic check yet |
| FMNMX R | stop | | no semantic check yet |
| FMNMX I | stop | | no semantic check yet |
| FMNMX C | stop | | no semantic check yet |
| FMUL R | stop | | no semantic check yet |
| FMUL I | stop | | no semantic check yet |
| FMUL C | stop | | no semantic check yet |
| FMUL 32I | stop | | no semantic check yet |
| FSET R | stop | | no semantic check yet |
| FSET I | stop | | no semantic check yet |
| FSET C | stop | | no semantic check yet |
| FSETP R | stop | | no semantic check yet |
| FSETP I | stop | | no semantic check yet |
| FSETP C | stop | | no semantic check yet |
| FSWZADD | stop | | no semantic check yet |
| GETCRSPTR | stop | | no semantic check yet |
| GETLMEMBASE | stop | | no semantic check yet |
| HADD2 R | stop | | no semantic check yet |
| HADD2 I | stop | | no semantic check yet |
| HADD2 C | stop | | no semantic check yet |
| HADD2 32I | stop | | no semantic check yet |
| HFMA2 R | stop | | no semantic check yet |
| HFMA2 I | stop | | no semantic check yet |
| HFMA2 C | stop | | no semantic check yet |
| HFMA2 RC | stop | | no semantic check yet |
| HFMA2 32I | stop | | no semantic check yet |
| HMUL2 R | stop | | no semantic check yet |
| HMUL2 I | stop | | no semantic check yet |
| HMUL2 C | stop | | no semantic check yet |
| HMUL2 32I | stop | | no semantic check yet |
| HSET2 R | stop | | no semantic check yet |
| HSET2 I | stop | | no semantic check yet |
| HSET2 C | stop | | no semantic check yet |
| HSETP2 R | stop | | no semantic check yet |
| HSETP2 I | stop | | no semantic check yet |
| HSETP2 C | stop | | no semantic check yet |
| I2F R | stop | | no semantic check yet |
| I2F I | stop | | no semantic check yet |
| I2F C | stop | | no semantic check yet |
| I2I R | stop | | no semantic check yet |
| I2I I | stop | | no semantic check yet |
| I2I C | stop | | no semantic check yet |
| IADD R | stop | | no semantic check yet |
| IADD I | stop | | no semantic check yet |
| IADD C | stop | | no semantic check yet |
| IADD 32I | stop | | no semantic check yet |
| IADD3 R | stop | | no semantic check yet |
| IADD3 I | stop | | no semantic check yet |
| IADD3 C | stop | | no semantic check yet |
| ICMP R | stop | | no semantic check yet |
| ICMP I | stop | | no semantic check yet |
| ICMP C | stop | | no semantic check yet |
| ICMP RC | stop | | no semantic check yet |
| IDE | stop | | no semantic check yet |
| IDP R | stop | | no semantic check yet |
| IDP C | stop | | no semantic check yet |
| IMAD R | stop | | no semantic check yet |
| IMAD I | stop | | no semantic check yet |
| IMAD C | stop | | no semantic check yet |
| IMAD RC | stop | | no semantic check yet |
| IMAD 32I | stop | | no semantic check yet |
| IMADSP R | stop | | no semantic check yet |
| IMADSP I | stop | | no semantic check yet |
| IMADSP C | stop | | no semantic check yet |
| IMADSP RC | stop | | no semantic check yet |
| IMNMX R | stop | | no semantic check yet |
| IMNMX I | stop | | no semantic check yet |
| IMNMX C | stop | | no semantic check yet |
| IMUL R | stop | | no semantic check yet |
| IMUL I | stop | | no semantic check yet |
| IMUL C | stop | | no semantic check yet |
| IMUL 32I | stop | | no semantic check yet |
| IPA | stop | | no semantic check yet |
| ISBERD | stop | | no semantic check yet |
| ISCADD R | stop | | no semantic check yet |
| ISCADD I | stop | | no semantic check yet |
| ISCADD C | stop | | no semantic check yet |
| ISCADD 32I | stop | | no semantic check yet |
| ISET R | stop | | no semantic check yet |
| ISET I | stop | | no semantic check yet |
| ISET C | stop | | no semantic check yet |
| ISETP R | stop | | no semantic check yet |
| ISETP I | stop | | no semantic check yet |
| ISETP C | stop | | no semantic check yet |
| JCAL | stop | | no semantic check yet |
| JMP | stop | | no semantic check yet |
| JMX | stop | | no semantic check yet |
| KIL | stop | | no semantic check yet |
| LD | stop | | no semantic check yet |
| LDC | stop | | no semantic check yet |
| LDG | stop | | no semantic check yet |
| LDL | stop | | no semantic check yet |
| LDS | stop | | no semantic check yet |
| LEA R | stop | | no semantic check yet |
| LEA I | stop | | no semantic check yet |
| LEA C | stop | | no semantic check yet |
| LEA_HI R | stop | | no semantic check yet |
| LEA_HI C | stop | | no semantic check yet |
| LEPC | stop | | no semantic check yet |
| LONGJMP | stop | | no semantic check yet |
| LOP R | stop | | no semantic check yet |
| LOP I | stop | | no semantic check yet |
| LOP C | stop | | no semantic check yet |
| LOP3 R | stop | | no semantic check yet |
| LOP3 I | stop | | no semantic check yet |
| LOP3 C | stop | | no semantic check yet |
| LOP 32I | stop | | no semantic check yet |
| MEMBAR | stop | | no semantic check yet |
| MOV R | stop | | no semantic check yet |
| MOV I | stop | | no semantic check yet |
| MOV C | stop | | no semantic check yet |
| MOV 32I | stop | | no semantic check yet |
| MUFU | stop | | no semantic check yet |
| NOP | stop | | no semantic check yet |
| OUT R | stop | | no semantic check yet |
| OUT I | stop | | no semantic check yet |
| OUT C | stop | | no semantic check yet |
| P2R R | stop | | no semantic check yet |
| P2R I | stop | | no semantic check yet |
| P2R C | stop | | no semantic check yet |
| PBK | stop | | no semantic check yet |
| PCNT | stop | | no semantic check yet |
| PEXIT | stop | | no semantic check yet |
| PIXLD | stop | | no semantic check yet |
| PLONGJMP | stop | | no semantic check yet |
| POPC R | stop | | no semantic check yet |
| POPC I | stop | | no semantic check yet |
| POPC C | stop | | no semantic check yet |
| PRET | stop | | no semantic check yet |
| PRMT R | stop | | no semantic check yet |
| PRMT I | stop | | no semantic check yet |
| PRMT C | stop | | no semantic check yet |
| PRMT RC | stop | | no semantic check yet |
| PSET | stop | | no semantic check yet |
| PSETP | stop | | no semantic check yet |
| R2B | stop | | no semantic check yet |
| R2P R | stop | | no semantic check yet |
| R2P I | stop | | no semantic check yet |
| R2P C | stop | | no semantic check yet |
| RAM | stop | | no semantic check yet |
| RED | stop | | no semantic check yet |
| RET | stop | | no semantic check yet |
| RRO R | stop | | no semantic check yet |
| RRO I | stop | | no semantic check yet |
| RRO C | stop | | no semantic check yet |
| RTT | stop | | no semantic check yet |
| S2R | stop | | no semantic check yet |
| SAM | stop | | no semantic check yet |
| SEL R | stop | | no semantic check yet |
| SEL I | stop | | no semantic check yet |
| SEL C | stop | | no semantic check yet |
| SETCRSPTR | stop | | no semantic check yet |
| SETLMEMBASE | stop | | no semantic check yet |
| SHF_L R | stop | | no semantic check yet |
| SHF_R R | stop | | no semantic check yet |
| SHF_L I | stop | | no semantic check yet |
| SHF_R I | stop | | no semantic check yet |
| SHFL | stop | | no semantic check yet |
| SHL R | stop | | no semantic check yet |
| SHL I | stop | | no semantic check yet |
| SHL C | stop | | no semantic check yet |
| SHR R | stop | | no semantic check yet |
| SHR I | stop | | no semantic check yet |
| SHR C | stop | | no semantic check yet |
| SSY | stop | | no semantic check yet |
| ST | stop | | no semantic check yet |
| STG | stop | | no semantic check yet |
| STL | stop | | no semantic check yet |
| STP | stop | | no semantic check yet |
| STS | stop | | no semantic check yet |
| SUATOM_B | stop | | no semantic check yet |
| SUATOM | stop | | no semantic check yet |
| SUATOM_B2 | stop | | no semantic check yet |
| SUATOM_CAS_B | stop | | no semantic check yet |
| SUATOM_CAS | stop | | no semantic check yet |
| SULD_DB | stop | | no semantic check yet |
| SULD_D | stop | | no semantic check yet |
| SULD_B | stop | | no semantic check yet |
| SULD | stop | | no semantic check yet |
| SURED_B | stop | | no semantic check yet |
| SURED | stop | | no semantic check yet |
| SUST_DB | stop | | no semantic check yet |
| SUST_D | stop | | no semantic check yet |
| SUST_B | stop | | no semantic check yet |
| SUST | stop | | no semantic check yet |
| SYNC | stop | | no semantic check yet |
| TEX | stop | | no semantic check yet |
| TEX_B | stop | | no semantic check yet |
| TEXS | stop | | no semantic check yet |
| TEXS_F16 | stop | | no semantic check yet |
| TLD | stop | | no semantic check yet |
| TLD_B | stop | | no semantic check yet |
| TLDS | stop | | no semantic check yet |
| TLDS_F16 | stop | | no semantic check yet |
| TLD4 | stop | | no semantic check yet |
| TLD4_B | stop | | no semantic check yet |
| TLD4S | stop | | no semantic check yet |
| TLD4S_F16 | stop | | no semantic check yet |
| TMML | stop | | no semantic check yet |
| TMML_B | stop | | no semantic check yet |
| TXA | stop | | no semantic check yet |
| TXD | stop | | no semantic check yet |
| TXD_B | stop | | no semantic check yet |
| TXQ | stop | | no semantic check yet |
| TXQ_B | stop | | no semantic check yet |
| VABSDIFF | stop | | no semantic check yet |
| VABSDIFF4 | stop | | no semantic check yet |
| VADD | stop | | no semantic check yet |
| VMAD | stop | | no semantic check yet |
| VMNMX | stop | | no semantic check yet |
| VOTE | stop | | no semantic check yet |
| VOTEVTG | stop | | no semantic check yet |
| VSET | stop | | no semantic check yet |
| VSETP | stop | | no semantic check yet |
| VSHL | stop | | no semantic check yet |
| VSHR | stop | | no semantic check yet |
| XMAD R | stop | | no semantic check yet |
| XMAD I | stop | | no semantic check yet |
| XMAD C | stop | | no semantic check yet |
| XMAD RC | stop | | no semantic check yet |

293 forms: 0 exact, 0 pseudo, 293 stop.
