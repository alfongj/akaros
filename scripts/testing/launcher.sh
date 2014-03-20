#!/bin/bash
# This script should be called from Jenkins when a new commit has been pushed 
# to the repo. 
# It analyzes what parts of the codebase have been modified, compiles everything
# that is needed, and reports on the results. 
readonly TEST_OUTPUT_DIR=output-tests
readonly TEST_DIR=scripts/testing

# Fill in new tests here
readonly TEST_NAMES=( kernel ) 



# TODO: Detect changes and fill TEST_NAMES and COMPILE_PARTS accordingly to the 
# ones needed

# TODO: Compile only the rules needed



# Create output directory for tests
mkdir -p $TEST_OUTPUT_DIR

# Run tests
for TEST_NAME in "${TEST_NAMES[@]}"
do
	`nosetests $TEST_NAME.py -w $TEST_DIR --with-xunit \
	--xunit-file=$TEST_OUTPUT_DIR/$TEST_NAME.xml`
done
