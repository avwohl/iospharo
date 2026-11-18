"pharo-llm/pharo-dataset: commit ff24157 of the package removed the method
CooCompletionDatasetExporter>>entriesAsCompletionJsonString (the JSON array
form of the entries) when the JSONL methods were rewritten.  The commit kept
two callers (writeToFileNamed: and writeToFileReference:) and the test
CooCompletionDatasetExporterTest>>testEntriesAsJsonStringSerializesEntries,
so the test and the two callers fail with doesNotUnderstand on every VM.
The repair restores the method with the body that the commit removed, but
reads filteredEntries (as the other entriesAs... methods now do) so that a
quality filter applies.  No test sends a request to a network service or reads
the environment."

add value: #CooCompletionDatasetExporter value: #entriesAsCompletionJsonString
	value: 'accessing'
	value:
'entriesAsCompletionJsonString

	^ NeoJSONWriter toString: (self filteredEntries collect: [ :entry |
		   Dictionary new
			   at: ''context'' put: (entry at: #context);
			   at: ''candidates'' put: (entry at: #candidates);
			   at: ''fetchers'' put: (entry at: #fetchers);
			   at: ''correct'' put: (entry at: #correct);
			   yourself ])'.
