Transcript show: 'VMMaker instance methods: '; cr.
(VMMaker methods collect: [:m | m selector]) sorted
    do: [:s | Transcript show: '  ', s; cr ].

Transcript cr.
Transcript show: 'PharoVMMaker instance methods: '; cr.
(PharoVMMaker methods collect: [:m | m selector]) sorted
    do: [:s | Transcript show: '  ', s; cr ].

Transcript cr.
Transcript show: 'VMMaker class methods: '; cr.
(VMMaker class methods collect: [:m | m selector]) sorted
    do: [:s | Transcript show: '  ', s; cr ].

Smalltalk exit: 0.
