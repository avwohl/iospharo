"procdiff.st -- scenarios run on our VM and on Cog; diff the two answers.
Run: test_load_image P.image eval "$(cat procdiff.st)"; for Cog, arch -x86_64 Pharo --headless C.image eval ...
See docs/vm-compat-bugs.md, 2026-10-17."

| log run results |
results := OrderedCollection new.
run := [:name :blk | log := OrderedCollection new. [ blk value: log ] on: Error do: [:e | log add: 'ERR ', e class name ]. results add: name -> log asArray ].
run value: #yieldOrder value: [:l | | ps | ps := (1 to: 3) collect: [:i | [ 1 to: 3 do: [:k | l add: i * 10 + k. Processor yield ] ] forkAt: Processor activePriority ]. 1 to: 12 do: [:k | Processor yield ]. l add: #done ].
run value: #higherPreempts value: [:l | [ l add: #high ] forkAt: Processor activePriority + 1. l add: #after ].
run value: #lowerWaits value: [:l | [ l add: #low ] forkAt: Processor activePriority - 1. l add: #after. Processor yield. l add: #afterYield. (Delay forMilliseconds: 20) wait. l add: #afterDelay ].
run value: #semOrder value: [:l | | s | s := Semaphore new. (1 to: 4) do: [:i | [ s wait. l add: i ] forkAt: Processor activePriority + 1 ]. 4 timesRepeat: [ s signal ]. l add: #end ].
run value: #suspendResume value: [:l | | p r | p := [ l add: #start. Processor activeProcess suspend. l add: #resumed ] forkAt: Processor activePriority + 1. l add: p isSuspended. r := p resume. l add: r == p. Processor yield. l add: p isTerminated ].
run value: #terminateEnsure value: [:l | | p | p := [ [ l add: #body. Semaphore new wait ] ensure: [ l add: #ensure ] ] forkAt: Processor activePriority + 1. l add: #forked. p terminate. l add: p isTerminated ].
run value: #terminateIfCurtailed value: [:l | | p | p := [ [ Semaphore new wait ] ifCurtailed: [ l add: #curtailed ] ] forkAt: Processor activePriority + 1. p terminate. l add: #t ].
run value: #mutex value: [:l | | m | m := Mutex new. (1 to: 3) do: [:i | [ m critical: [ l add: i. Processor yield. l add: i negated ] ] forkAt: Processor activePriority ]. 10 timesRepeat: [ Processor yield ]. l add: #end ].
run value: #waitTimeout value: [:l | | s | s := Semaphore new. l add: (s waitTimeoutMilliseconds: 10). s signal. l add: (s waitTimeoutMilliseconds: 10) ].
run value: #priorityChange value: [:l | | p | p := [ l add: #ran ] forkAt: Processor activePriority - 1. p priority: Processor activePriority + 1. l add: #after ].
run value: #signalOrderMixedPrio value: [:l | | s | s := Semaphore new. #(31 33 32) do: [:pr | [ s wait. l add: pr ] forkAt: pr ]. Processor yield. 3 timesRepeat: [ s signal ]. (Delay forMilliseconds: 10) wait. l add: #end ].
run value: #processLocal value: [:l | | p | p := [ l add: (Processor activeProcess psValueAt: 1) ] newProcess. l add: p isSuspended. p resume. Processor yield. l add: #done ].
run value: #exceptionInFork value: [:l | | p | p := [ [ 1/0 ] on: ZeroDivide do: [:e | l add: #caught. e return: 5 ] ] forkAt: Processor activePriority + 1. l add: #main ].
run value: #suspendedContext value: [:l | | p | p := [ Semaphore new wait ] forkAt: Processor activePriority + 1. l add: p suspendedContext method selector. l add: (p suspendedContext sender ifNotNil: [:c | c method selector]). p terminate ].
run value: #yieldNoOthers value: [:l | 3 timesRepeat: [ Processor yield ]. l add: #ok ].
run value: #activeProcessIdentity value: [:l | | p q | p := Processor activeProcess. [ q := Processor activeProcess ] forkAt: p priority + 1. l add: (p == q) ].
results asArray
