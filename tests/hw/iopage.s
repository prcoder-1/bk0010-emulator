/ Какие адреса страницы В-В (0177600–0177776) отвечают на шине БК-0010(-01).
/ Каждое слово читается (TST) с перехватом вектора 4; где чтение ушло в ловушку,
/ пробуется запись нуля (MOV, без чтения). На экране по строке на 8 слов:
/ «.» — ответ, «X» — зависание (вектор 4), «-» — запись не проверялась.
/ Эмулятор (до 2026-11-20) читает неизвестные адреса как 0 и глотает записи;
/ GID, bkbtl и kromych/BK-0011M дают ловушку.
/
/ Сборка:
/   pdp11-aout-as -o iopage.o iopage.s
/   pdp11-aout-ld -Ttext 0x200 -o iopage.out iopage.o
/   pdp11-aout-objcopy -O binary iopage.out iopage.raw
/   python3 mkbin.py iopage.raw IOPAGE.BIN

	.text
	.globl	start
start:	mov	$01000, sp
	mov	$014, r0		/ очистить экран
	emt	016
	mov	$title, r4
	jsr	pc, puts
	mov	*$4, -(sp)		/ сохранить вектор 4 (СТОП)
	mov	*$6, -(sp)
	mov	$trap4, *$4
	mov	$0340, *$6
	mov	$0177600, r5
row:	mov	r5, r0
	jsr	pc, putoct
	mov	$010, r3
cell:	mov	$040, r0
	emt	016
	clr	flag
	tst	(r5)
	mov	flag, r4		/ 1 — чтение ушло в вектор 4
	mov	$056, r0
	tst	r4
	beq	1f
	mov	$0130, r0
1:	emt	016
	mov	$055, r0
	tst	r4
	beq	2f
	clr	flag
	mov	$0, (r5)
	mov	$056, r0
	tst	flag
	beq	2f
	mov	$0130, r0
2:	emt	016
	add	$2, r5
	sob	r3, cell
	mov	$012, r0
	emt	016
	tst	r5
	bne	row
	mov	(sp)+, *$6
	mov	(sp)+, *$4
	mov	$done, r4
	jsr	pc, puts
3:	br	3b

trap4:	mov	$1, flag
	rti

/ Вывод строки по R4 (до нуля)
puts:	movb	(r4)+, r0
	beq	1f
	emt	016
	br	puts
1:	rts	pc

/ Вывод R0 шестью восьмеричными цифрами
putoct:	mov	r0, r1
	clr	r0
	asl	r1
	rol	r0
	add	$060, r0
	emt	016
	mov	$5, r2
1:	clr	r0
	asl	r1
	rol	r0
	asl	r1
	rol	r0
	asl	r1
	rol	r0
	add	$060, r0
	emt	016
	sob	r2, 1b
	rts	pc

flag:	.word	0
title:	.asciz	"IO PAGE: READ WRITE, .=OK X=TRAP\n"
done:	.asciz	"DONE\n"
	.even
