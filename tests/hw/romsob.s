/ Задержка ответа ПЗУ К1801РЕ2: время одного прохода петли `SOB R0,.`, целиком
/ исполняемой из ПЗУ монитора (подпрограмма 102102: MOV R3,R0 / SOB R0,. / DEC R3 /
/ SOB R2,... / MOV @#177716,R0 / MOVB @#177662,R0 / RTS PC; при R2=1 внешняя петля
/ не повторяется). Два прогона с N=1000 и N=13800 проходов; разность в отсчётах
/ таймера (128 тактов) на 12800 проходов — ровно такты на проход в сотых.
/ Для сравнения та же подпрограмма, скопированная в ДОЗУ, и опорная строка фазы
/ `TST @#1000` (36 — фаза 0, 32 — фаза 1), как в ROMIO.BIN.
/
/ Сборка — как у romio.s (tests/hw/mkbin.py).

	.text
	.globl	start
start:	mov	$01000, sp
	mov	$014, r0		/ очистить экран
	emt	016
	mov	$title, r4
	jsr	pc, puts
/ Опорная строка: 1280 копий TST @#1000
	mov	$nph, r4
	jsr	pc, puts
	mov	$build1, r4
	jsr	pc, mkcode
	jsr	pc, *$codmem
	jsr	pc, put10
/ Копия подпрограммы в ДОЗУ
	mov	$0102102, r0
	mov	$ramsub, r1
1:	mov	(r0)+, (r1)+
	cmp	r0, $0102124
	blo	1b
/ ПЗУ
	mov	$nrom, r4
	jsr	pc, puts
	mov	$0102102, r1
	jsr	pc, sobrun
/ ДОЗУ
	mov	$nram, r4
	jsr	pc, puts
	mov	$ramsub, r1
	jsr	pc, sobrun
done:	br	done

/ Такты на проход петли подпрограммы по адресу R1, печать в сотых
sobrun:	mov	r1, call+2
	mov	$build2, r4
	jsr	pc, mkcode
	mov	$1000, r3
	mov	$1, r2
	jsr	pc, *$codmem
	mov	r0, -(sp)
	mov	$13800, r3
	mov	$1, r2
	jsr	pc, *$codmem
	sub	(sp)+, r0
	jsr	pc, put100
	rts	pc

/ codmem: пуск таймера, тело по таблице R4 (пары адрес начала/конца, число
/ повторов), останов таймера
mkcode:	mov	$tstart, r0
	mov	$codmem, r2
1:	mov	(r0)+, (r2)+
	cmp	r0, $tstop
	blo	1b
	mov	(r4)+, r1		/ начало тела
	mov	(r4)+, r3		/ конец тела
	mov	(r4)+, r5		/ повторов
2:	mov	r1, r0
3:	mov	(r0)+, (r2)+
	cmp	r0, r3
	blo	3b
	sob	r5, 2b
	mov	$tstop, r0
4:	mov	(r0)+, (r2)+
	cmp	r0, $tend
	blo	4b
	rts	pc

build1:	.word	tst1, tst1e, 1280
build2:	.word	call, calle, 1
tst1:	tst	*$01000
tst1e:
call:	jsr	pc, *$0102102
calle:

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

/ Печать R0 как «ddd.d» (put10) или «ddd.dd» (put100) и перевод строки
put10:	mov	$10, r1
	mov	$1, r3
	br	putfx
put100:	mov	$100, r1
	mov	$2, r3
putfx:	clr	r2			/ целая часть
1:	cmp	r0, r1
	blo	2f
	sub	r1, r0
	inc	r2
	br	1b
2:	mov	r0, -(sp)		/ дробная часть
	mov	r3, -(sp)		/ знаков после точки
	mov	r2, r0
	jsr	pc, putdec
	mov	$056, r0
	emt	016
	mov	(sp)+, r3
	mov	(sp)+, r0
	cmp	r3, $1
	beq	3f
	cmp	r0, $10			/ ведущий ноль у сотых
	bhis	3f
	mov	r0, -(sp)
	mov	$060, r0
	emt	016
	mov	(sp)+, r0
3:	jsr	pc, putdec
	mov	$012, r0
	emt	016
	rts	pc

/ Десятичная печать R0 без ведущих нулей
putdec:	mov	$pow10, r4
	clr	r3			/ уже печатали цифру
1:	mov	(r4)+, r1
	beq	4f
	mov	$060, r2
2:	cmp	r0, r1
	blo	3f
	sub	r1, r0
	inc	r2
	br	2b
3:	cmp	r2, $060
	bne	33f
	tst	r3
	bne	33f
	cmp	r1, $1
	bne	1b
33:	mov	r0, -(sp)
	mov	r2, r0
	emt	016
	mov	(sp)+, r0
	inc	r3
	br	1b
4:	rts	pc

pow10:	.word	10000, 1000, 100, 10, 1, 0

title:	.asciz	"ROM SOB LOOP (CLOCKS)\n"
nph:	.asciz	"TST @#1000        "
nrom:	.asciz	"SOB R0,. IN ROM   "
nram:	.asciz	"SOB R0,. IN RAM   "
	.even
ramsub:	.word	0, 0, 0, 0, 0, 0, 0, 0, 0, 0

	codmem = 020000
