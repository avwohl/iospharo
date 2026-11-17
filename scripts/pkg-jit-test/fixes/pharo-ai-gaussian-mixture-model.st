"pharo-ai/gaussian-mixture-model: four tests of AIGaussianMixtureModelTest stop with
PMSingularMatrixError (lupInverse encountered a singular matrix): testFitX, testLabels,
testPredictNewData, testPredictSameFittingData.  The two VMs give the same result.

Cause.  The package fits the mixture with the EM algorithm and never regularises the
covariance matrices.  The test data has an exactly degenerate shape.  In testLabels,
testPredictNewData and testPredictSameFittingData the points are (1 2) (1 4) (1 0) and
(10 2) (10 4) (10 0): in each cluster the x value is the same for all points, so the
maximum-likelihood covariance matrix of a cluster has a variance of 0 in x.  In testFitX
one Gaussian collapses on the two points with x = 0.5 and gets the same zero variance.
A covariance matrix with a zero row has no inverse: the LUP decomposition finds the pivot
0, and PMMultivariateNormalDistribution>>value: sends inverse.  The standard remedy, also
used by scikit-learn (reg_covar = 1e-6), is to add a small multiple of the identity matrix
to each covariance matrix in the M step.  A hand run of the algorithm in Python with the
exact Random generator of Pharo 14 (PCG32, seed 3) shows the singular matrix in the
second M step for both data sets, and no singular matrix with the regularisation.
The tests passed in 2022 only by luck of the old Random generator (Pharo 9 and 10): the
seed 3 gave other start points.  Pharo 14 has another generator, so the start points are
other points, and the degenerate fit is reached.

Second cause, found while the first was repaired.  With a variance of 1e-6 the density of
a point that is far from a cluster is below the smallest Float (exp of -500000 is 0.0).
For the point (0 0) both densities are 0.0, and calculateResponsibilitiesFor: divides 0.0
by 0.0 and answers NaN.  labels then answers 0 (a NaN is not equal to the maximum).  The
old expected values #( 1 1 1 0 0 0 ) and #( 1 0 ) contain this 0: a label is a number
from 1 to the number of Gaussians, so 0 is no cluster, it is an artefact of NaN.
calculateResponsibilitiesFor: now computes in the log domain (log-sum-exp), the usual
remedy.  A new method logProbabilityOfVector:beingGeneratedByDistributionAt: gives the
log density and converts a plain Array to a PMVector (predict: gets Arrays).

Third cause.  AIGaussianMixtureModel>>labels answered an OrderedCollection (the
responsibilities are an OrderedCollection, because fitX: makes one), and the test
compares with an Array.  An OrderedCollection is not equal to an Array, so testLabels
could never pass.  labels answers an Array now, as predict: does.

Test repair.  The three tests compare with labels that depend on the random start
(which Gaussian gets which cluster) and on the NaN artefact.  The new test bodies assert
what the model must do: the first three points get one label, the last three get another
label, both labels are 1 or 2, predict: of the fitted data answers the labels of the fit,
and the new points (0 0) and (12 3) get the label of the cluster at x = 1 and of the
cluster at x = 10.  No assertion is dropped without a replacement.  The hand run gives
labels #( 1 1 1 2 2 2 ) and #( 1 2 ) for the new points.

The entries change package methods, so the coordinator can instead send them upstream
as a patch of pharo-ai/gaussian-mixture-model."

add value: #'AIGaussianMixtureModel class' value: #defaultCovarianceRegularization
	value: 'accesing - defaults'
	value:
'defaultCovarianceRegularization

	"A small multiple of the identity matrix, added to each covariance matrix in the M step, so that a cluster of points with an equal coordinate does not get a singular covariance matrix (scikit-learn reg_covar)."

	^ 1e-6'.

add value: #AIGaussianMixtureModel value: #logProbabilityOfVector:beingGeneratedByDistributionAt:
	value: 'fitting'
	value:
'logProbabilityOfVector: aVector beingGeneratedByDistributionAt: anIndex

	"Answer the logarithm of the density of the distribution at anIndex for aVector.  The logarithm does not underflow for a vector that is far from the mean."

	| distribution difference |
	distribution := gaussians at: anIndex.
	difference := aVector asPMVector - distribution meanVector.
	^ (-1 / 2 * difference * distribution covarianceMatrix inverse * difference)
		- ((((2 * Float pi) raisedTo: distribution power) * distribution covarianceMatrix determinant) ln / 2)'.

fix value: #AIGaussianMixtureModel value: #calculateResponsibilitiesFor:
	value: [ :source | (source includesSubstring: 'logProbabilityOfVector') not ]
	value:
'calculateResponsibilitiesFor: aCollectionOfVectors

	"Answer the responsibilities of each Gaussian for each vector.  The sums run in the log domain, so that a vector with a density of 0.0 under each Gaussian does not give 0.0 / 0.0."

	^ aCollectionOfVectors collect: [ :each |
		| logWeights largest weights total |
		logWeights := (1 to: numberOfGaussians) collect: [ :k |
			(mixingCoefficients at: k) ln
				+ (self logProbabilityOfVector: each beingGeneratedByDistributionAt: k) ].
		largest := logWeights max.
		weights := logWeights collect: [ :each2 | (each2 - largest) exp ].
		total := weights sum.
		weights collect: [ :weight | weight / total ] ]'.

fix value: #AIGaussianMixtureModel value: #updateParametersFor:
	value: [ :source | (source includesSubstring: 'defaultCovarianceRegularization') not ]
	value:
'updateParametersFor: aCollectionOfVectors

	| n mu deviation sigma regularization |

	regularization := (PMSymmetricMatrix identity: aCollectionOfVectors anyOne size)
		* self class defaultCovarianceRegularization.

	1 to: numberOfGaussians do: [ :k |
		n := (responsibilities collect: [ :each | each at: k ]) sum.
		mu := (1 / n * ((1 to: aCollectionOfVectors size) collect: [ :i |
			((responsibilities at: i) at: k) * (aCollectionOfVectors at: i) ]) sum).

		sigma := 1 / n * ((1 to: aCollectionOfVectors size) collect: [ :i |
			deviation := (aCollectionOfVectors at: i) - mu.
			((responsibilities at: i) at: k) * (deviation tensorProduct: deviation) ]) sum.

		(gaussians at: k) initializeMeanVector: mu covarianceMatrix: sigma + regularization.

		mixingCoefficients at: k put: (n / aCollectionOfVectors size) ]'.

fix value: #AIGaussianMixtureModel value: #labels
	value: [ :source | (source includesSubstring: 'asArray') not ]
	value:
'labels

	^ (responsibilities collect: [ :each | each indexOf: each max ]) asArray'.

add value: #AIGaussianMixtureModelTest value: #assertLabelsSeparateTheTwoColumns:
	value: 'asserting'
	value:
'assertLabelsSeparateTheTwoColumns: labels

	"The points of the fixture are three with x = 1 and three with x = 10.  The number of a cluster depends on the random start, so the test asks for two different labels, equal inside each group of three."

	self assert: labels size equals: 6.
	self assert: (labels copyFrom: 1 to: 3) asArray equals: (Array new: 3 withAll: labels first).
	self assert: (labels copyFrom: 4 to: 6) asArray equals: (Array new: 3 withAll: labels last).
	self deny: labels first = labels last.
	self assert: (labels allSatisfy: [ :each | each between: 1 and: 2 ])'.

fix value: #AIGaussianMixtureModelTest value: #testFitX
	value: [ :source | (source includesSubstring: 'mixingCoefficients') not ]
	value:
'testFitX

	model numberOfGaussians: 2.
	model fitX: data.

	self assert: model mixingCoefficients sum closeTo: 1.
	self deny: model logLikelihood isNaN'.

fix value: #AIGaussianMixtureModelTest value: #testLabels
	value: [ :source | (source includesSubstring: 'assertLabelsSeparateTheTwoColumns') not ]
	value:
'testLabels

	model numberOfGaussians: 2.
	model fitX: #( #( 1 2 ) #( 1 4 ) #( 1 0 ) #( 10 2 ) #( 10 4 ) #( 10 0 ) ).

	self assertLabelsSeparateTheTwoColumns: model labels'.

fix value: #AIGaussianMixtureModelTest value: #testPredictNewData
	value: [ :source | (source includesSubstring: 'assertLabelsSeparateTheTwoColumns') not ]
	value:
'testPredictNewData

	| labels |
	model numberOfGaussians: 2.
	model fitX: #( #( 1 2 ) #( 1 4 ) #( 1 0 ) #( 10 2 ) #( 10 4 ) #( 10 0 ) ).
	labels := model labels.
	self assertLabelsSeparateTheTwoColumns: labels.

	"(0 0) is near the cluster with x = 1, (12 3) is near the cluster with x = 10"
	self assert: (model predict: #( #( 0 0 ) #( 12 3 ) )) equals: { labels first. labels last }'.

fix value: #AIGaussianMixtureModelTest value: #testPredictSameFittingData
	value: [ :source | (source includesSubstring: 'assertLabelsSeparateTheTwoColumns') not ]
	value:
'testPredictSameFittingData

	| labels |
	model numberOfGaussians: 2.
	model fitX: #( #( 1 2 ) #( 1 4 ) #( 1 0 ) #( 10 2 ) #( 10 4 ) #( 10 0 ) ).
	labels := model labels.
	self assertLabelsSeparateTheTwoColumns: labels.

	self
		assert: (model predict: #( #( 1 2 ) #( 1 4 ) #( 1 0 ) #( 10 2 ) #( 10 4 ) #( 10 0 ) ))
		equals: labels'.
