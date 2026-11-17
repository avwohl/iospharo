"RomaShmyhelskyi/PharoAthleteProject, branch master (see the .md: the manifest
row must load branch master, because the baseline of branch main has its
baseline: method on the class side and loads no package at all).

TennisPlayer>>addPrizeMoney: did addFirst: and then removeLast at every call.
With fewer than 3 prizes in the list, the call removed the prize that it had
just added, so the list stayed empty.  TennisPlayerTest testAddSinglePrizeMoney
(expects size 1), testAddMultiplePrizeMoney (size 3), testKeepOnlyLastThreePrizes
and the two compare tests (all salaries were 0) could not pass.  The class
comment says that the oldest prize leaves only when a fourth one comes in.  The
repair removes the last element only while the list has more than 3.

Footballer>>goalsScored: refused 0 (test aNumber > 0), but a footballer with no
goal is valid: the initialize method sets 0 and the class comment counts goals
per year.  FootballerTest testCalculateIncomeWithoutGoals calls goalsScored: 0
and got an error.  The error text also did concatenate a number to a string
(String , Integer fails), so the refusal printed a second error.  The repair
accepts 0 and negative integers get an error with printString."

fix value: #TennisPlayer value: #addPrizeMoney:
	value: [ :source | (source includesSubstring: 'size > 3') not ]
	value:
'addPrizeMoney: amount
	"Додає призові з нового турніру. Зберігає тільки останні 3 турніри"

	amount isNumber
		ifTrue: [
			amount > 0
				ifTrue: [
					lastThreePrizeMoney addFirst: amount.
					[ lastThreePrizeMoney size > 3 ] whileTrue: [ lastThreePrizeMoney removeLast ] ]
				ifFalse: [ self error: ''Хибне значення: '' , amount printString ] ]
		ifFalse: [
			self error:
				''Недопустиме значення призових з останнього турніру: '' , amount printString ]'.

fix value: #Footballer value: #goalsScored:
	value: [ :source | (source includesSubstring: '>= 0') not ]
	value:
'goalsScored: aNumber
	"Встановлює кількість забитих голів"

	aNumber isInteger
		ifTrue: [
			aNumber >= 0
				ifTrue: [ goalsScored := aNumber ]
				ifFalse: [
				self error: ''Хибне значення кількості забитих голів: '' , aNumber printString ] ]
		ifFalse: [
			self error:
				''Недопустиме значення кількості забитих голів: '' , aNumber printString ].
	^ aNumber'.
