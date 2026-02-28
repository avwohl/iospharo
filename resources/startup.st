"Pharo 13 image patches — loaded automatically by StartupPreferencesLoader.
These fix known bugs in the Pharo 13 image that affect all VMs."

"Fix: Microdown doc browser crashes on fresh images because MicGitHubAPI uses
IceTokenCredentials with placeholder 'YOUR TOKEN', causing 401 responses from
GitHub without rate-limit headers. extractRateInfo: then fails with KeyNotFound.
Fix: use anonymous API access (no auth needed for public repos)."
(Smalltalk hasClassNamed: #MicGitHubRessourceReference) ifTrue: [
  MicGitHubRessourceReference compile: 'githubApi
    ^ MicGitHubAPI new beAnonymous'].
