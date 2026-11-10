.text
.balign 16
.globl main
main:
	endbr64
	pushq %rbp
	movq %rsp, %rbp
	movl $0, %eax
	movl $1, %ecx
.p2align 4
.Lbb2:
	cmpl $11, %ecx
	jge .Lbb4
	addl %ecx, %eax
	addl $1, %ecx
	jmp .Lbb2
.Lbb4:
	movl %eax, %edi
	callq print_int
	movl $0, %eax
	leave
	ret
.type main, @function
.size main, .-main
/* end function main */

.section .note.GNU-stack,"",@progbits
