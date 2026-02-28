"Fix Microdown doc browser: use anonymous GitHub API to avoid IceTokenCredentials errors"!

!MicGitHubRessourceReference methodsFor: 'private'!
githubApi
  ^ MicGitHubAPI new beAnonymous! !
