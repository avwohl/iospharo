"locdiff.st -- LocalePlugin answers for a diff against Cog.  2026-10-17: DST, metric,
separators, currency and zone agree; the format strings differ on purpose (ours are
translated to the image's format language, Cog passes ICU patterns) and Cog ends
language and country with a NUL."

| r l |
r := OrderedCollection new.
l := Locale current.
#(#primDST #primDecimalSymbol #primTimeFormat #primDigitGrouping #primLanguage #primMeasurement #primCurrencySymbol #primCountry #primTimezone #primShortDateFormat #primLongDateFormat #primCurrencyNotation #primVMOffsetToUTC) do: [:s |
  r add: s -> ([ (l perform: s) printString ] on: Error do: [:e | 'ERR ', e class name ]) ].
r add: #tzOffset -> ([ LocalTimeZone new primOffset printString ] on: Error do: [:e | 'ERR ', e class name ]).
r asArray
