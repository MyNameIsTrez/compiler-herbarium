.text
.balign 16
fact:
	endbr64
	pushq %rbp
	movq %rsp, %rbp
	subq $8, %rsp
	pushq %rbx
	movl %esi, %eax
	cmpl $2, %edi
	jl .Lbb2
	movl %edi, %ebx
	subl $1, %edi
	movl %eax, %esi
	callq fact
	movl %ebx, %edi
	imull %edi, %eax
.Lbb2:
	popq %rbx
	leave
	ret
.type fact, @function
.size fact, .-fact
/* end function fact */

.text
.balign 16
.globl main
main:
	endbr64
	pushq %rbp
	movq %rsp, %rbp
	movl $1, %esi
	movl $5, %edi
	callq fact
	movl %eax, %edi
	callq print_int
	movl $0, %eax
	leave
	ret
.type main, @function
.size main, .-main
/* end function main */

.section .note.GNU-stack,"",@progbits
