
cd scugic_v2_1/src/
make scugic_includes
make libs EXTRA_COMPILER_FLAGS='-DUSE_AMP=1'
cd ../..
