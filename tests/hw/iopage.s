/ Какие адреса страницы В-В (0177600–0177776) отвечают на шине БК-0010(-01).
/ Каждое слово читается (TST) с перехватом вектора 4; где чтение ушло в ловушку,
/ пробуется запись нуля (MOV, без чтения). На экране по строке на 8 слов:
/ «.» — ответ, «X» — зависание (вектор 4), «-» — запись не проверялась.
/ БК-0010-01 пользователя (2026-11-20, через Gryphon-MPI): отвечают только
/ 0177660–0177664, 0177700–0177716 и 0177734 (регистр самой Gryphon-MPI), остальное —
/ «X X». Эмулятор до этого читал неизвестные адреса как 0 и глотал записи.
/
/ Монитор не нужен (под Gryphon-MPI его EMT после меню не работает): прерывания
/ запрещены, скролл 01330, экран 32 колонки, шрифт — font.s (mkfont.py из ПЗУ).
/
/ Сборка:
/   python3 mkfont.py ../../roms/monit10.rom font.s
/   pdp11-aout-as -o iopage.o iopage.s
/   pdp11-aout-ld -Ttext 0x200 -o iopage.out iopage.o
/   pdp11-aout-objcopy -O binary iopage.out iopage.raw
/   python3 mkbin.py iopage.raw IOPAGE.BIN

	.text
	.globl	start
start:	mtps	$0340
	mov	$01000, sp
	mov	$01330, *$0177664
	mov	$040000, r0		/ очистить экран
1:	clr	(r0)+
	cmp	r0, $0100000
	blo	1b
	mov	$040000+640, line	/ со второй строки текста
	clr	col
	mov	$title, r4
	jsr	pc, puts
	mov	$trap4, *$4
	mov	$0340, *$6
	mov	$0177600, r5
row:	mov	r5, r0
	jsr	pc, putoct
	mov	$010, r3
cell:	mov	$040, r0
	jsr	pc, putc
	clr	flag
	tst	(r5)
	mov	flag, r4		/ 1 — чтение ушло в вектор 4
	mov	$056, r0
	tst	r4
	beq	1f
	mov	$0130, r0
1:	jsr	pc, putc
	mov	$055, r0
	tst	r4
	beq	2f
	clr	flag
	mov	$0, (r5)
	mov	$056, r0
	tst	flag
	beq	2f
	mov	$0130, r0
2:	jsr	pc, putc
	add	$2, r5
	sob	r3, cell
	mov	$012, r0
	jsr	pc, putc
	tst	r5
	bne	row
	mov	$done, r4
	jsr	pc, puts
3:	br	3b

trap4:	mov	$1, flag
	rti

/ Символ R0 (040..0137, 012 — перевод строки) в позицию line/col
putc:	mov	r1, -(sp)
	mov	r2, -(sp)
	cmp	r0, $012
	bne	1f
	add	$640, line
	clr	col
	br	3f
1:	sub	$040, r0
	mov	r0, r1			/ R1 = глиф: font + 10 * (код - 040)
	asl	r1
	mov	r1, r2
	asl	r1
	asl	r1
	add	r2, r1
	add	$font, r1
	mov	col, r2			/ R2 = адрес на экране
	asl	r2
	add	line, r2
	mov	$10, -(sp)
2:	movb	(r1)+, r0
	bic	$0177400, r0
	asl	r0
	mov	dbl(r0), (r2)
	add	$64, r2
	dec	(sp)
	bne	2b
	tst	(sp)+
	inc	col
3:	mov	(sp)+, r2
	mov	(sp)+, r1
	rts	pc

/ Вывод строки по R4 (до нуля)
puts:	movb	(r4)+, r0
	beq	1f
	jsr	pc, putc
	br	puts
1:	rts	pc

/ Вывод R0 шестью восьмеричными цифрами
putoct:	mov	r0, r1
	clr	r0
	asl	r1
	rol	r0
	add	$060, r0
	mov	r1, -(sp)
	jsr	pc, putc
	mov	(sp)+, r1
	mov	$5, r2
1:	clr	r0
	asl	r1
	rol	r0
	asl	r1
	rol	r0
	asl	r1
	rol	r0
	add	$060, r0
	mov	r1, -(sp)
	mov	r2, -(sp)
	jsr	pc, putc
	mov	(sp)+, r2
	mov	(sp)+, r1
	sob	r2, 1b
	rts	pc

flag:	.word	0
line:	.word	0
col:	.word	0
title:	.asciz	"IO PAGE: READ WRITE\n.=OK X=TRAP -=NOT TESTED\n\n"
done:	.asciz	"DONE\n"
	.even
	.include "font.s"
