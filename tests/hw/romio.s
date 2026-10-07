/ Замер времени команд, обращающихся к ПЗУ и регистрам В-В, на БК-0010(-01).
/ Метод — как у 45com (©Manwe): 1280 копий команды подряд в ДОЗУ между пуском и
/ остановом таймера 0177710; таймер считает раз в 128 тактов, так что результат —
/ время одной команды в десятых долях такта. Предсказания модели —
/ docs/slow-memory-timing.md (две фазы включения питания).
/
/ Сборка:
/   pdp11-aout-as -o romio.o romio.s
/   pdp11-aout-ld -Ttext 0x200 -o romio.out romio.o
/   pdp11-aout-objcopy -O binary romio.out romio.raw
/   + заголовок .BIN: адрес 01000 и длина (см. tests/hw/mkbin.py)

	.text
	.globl	start
start:	mov	$01000, sp
	mov	$014, r0		/ очистить экран
	emt	016
	mov	$title, r4
	jsr	pc, puts
	mov	$cases, r5
next:	mov	(r5)+, r4		/ имя случая
	beq	done
	jsr	pc, puts
	mov	(r5)+, r3		/ слов в команде
	mov	(r5)+, r0
	mov	(r5)+, r1
	mov	(r5)+, -(sp)		/ R1 на время замера
	mov	r5, -(sp)
	jsr	pc, build
	mov	(sp)+, r5
	mov	(sp)+, r1
	mov	r5, -(sp)
	jsr	pc, *$codmem		/ R0 — время в десятых такта
	mov	(sp)+, r5
	jsr	pc, putnum
	br	next
done:	br	done

/ Код замера в codmem: пуск таймера, 1280 копий команды (R0, R1 — её слова, R3 —
/ число слов), останов таймера.
build:	mov	$tstart, r4
	mov	$codmem, r2
1:	mov	(r4)+, (r2)+
	cmp	r4, $tstop
	blo	1b
	mov	$1280, r4
2:	mov	r0, (r2)+
	cmp	r3, $1
	beq	3f
	mov	r1, (r2)+
3:	sob	r4, 2b
	mov	$tstop, r4
4:	mov	(r4)+, (r2)+
	cmp	r4, $tend
	blo	4b
	rts	pc

tstart:	mov	$1, *$0177706		/ предел таймера
	mov	$1, *$0177712		/ стоп и загрузка
	mov	$0177710, r5
	mtps	$0340			/ прерывания запрещены
	mov	$032, *$0177712		/ пуск
1:	tst	(r5)			/ дождаться, пока таймер пошёл
	bne	1b
tstop:	mov	(r5), r0
	neg	r0
	mtps	$0
	rts	pc
tend:

/ Вывод строки по R4 (до нуля)
puts:	movb	(r4)+, r0
	beq	1f
	emt	016
	br	puts
1:	rts	pc

/ Вывод R0 (десятые доли) как «ddd.d» и перевод строки
putnum:	clr	r2			/ целая часть
1:	cmp	r0, $10
	blo	2f
	sub	$10, r0
	inc	r2
	br	1b
2:	mov	r0, -(sp)		/ десятые
	mov	r2, r0
	mov	$pow10, r4
	clr	r3			/ уже печатали цифру
3:	mov	(r4)+, r1
	beq	6f
	mov	$060, r2
4:	cmp	r0, r1
	blo	5f
	sub	r1, r0
	inc	r2
	br	4b
5:	cmp	r2, $060
	bne	55f
	tst	r3
	bne	55f
	cmp	r1, $1
	bne	3b
55:	mov	r0, -(sp)
	mov	r2, r0
	emt	016
	mov	(sp)+, r0
	inc	r3
	br	3b
6:	mov	$056, r0
	emt	016
	mov	(sp)+, r0
	add	$060, r0
	emt	016
	mov	$012, r0
	emt	016
	rts	pc

pow10:	.word	10000, 1000, 100, 10, 1, 0

/ Случаи: имя, слов, слово 0, слово 1, начальный R1
cases:	.word	n1, 2, 005737, 001000, 0	/ TST @#1000 — опорный, ДОЗУ
	.word	n2, 2, 005737, 0100000, 0	/ TST @#100000 — ПЗУ
	.word	n3, 2, 005737, 0177716, 0	/ TST @#177716 — В-В
	.word	n4, 2, 013701, 0100000, 0	/ MOV @#100000,R1
	.word	n5, 2, 010037, 0177714, 0	/ MOV R0,@#177714
	.word	n6, 2, 005037, 0177714, 0	/ CLR @#177714
	.word	n7, 2, 030037, 0177716, 0	/ BIT R0,@#177716
	.word	n8, 1, 0111100, 0, 0100000	/ MOVB (R1),R0, R1 в ПЗУ
	.word	n9, 1, 0111100, 0, 001000	/ MOVB (R1),R0, R1 в ДОЗУ
	.word	n10, 2, 004737, 0100256, 0	/ JSR PC,@#100256 — там RTS PC в ПЗУ
	.word	0

title:	.asciz	"ROM/IO TIMING (CLOCKS)\n"
n1:	.asciz	"TST @#1000      "
n2:	.asciz	"TST @#100000    "
n3:	.asciz	"TST @#177716    "
n4:	.asciz	"MOV @#100000,R1 "
n5:	.asciz	"MOV R0,@#177714 "
n6:	.asciz	"CLR @#177714    "
n7:	.asciz	"BIT R0,@#177716 "
n8:	.asciz	"MOVB (R1),R0 ROM "
n9:	.asciz	"MOVB (R1),R0 RAM "
n10:	.asciz	"JSR PC,@#100256  "
	.even

	codmem = 020000
