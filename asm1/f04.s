	.file	"f04.cc"
	.text
	.globl	_Z1fv
	.type	_Z1fv, @function
_Z1fv:
.LFB0:
	movl	a(%rip), %eax
	ret
.LFE0:
	.size	_Z1fv, .-_Z1fv
	.globl	a
	.data
	.align 4
	.type	a, @object
	.size	a, 4
a:
	.long	10
	.ident	"GCC: (Ubuntu 7.4.0-1ubuntu1~18.04.1) 7.4.0"
	.section	.note.GNU-stack,"",@progbits
