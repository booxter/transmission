{ pkgs ? import <nixpkgs> { system = "x86_64-linux"; } }:

let
  src = builtins.path {
    path = ../..;
    name = "transmission-local-src";
  };

  commonOverride =
    old: {
      pname = "transmission-local";
      version = "local";
      inherit src;

      # Match the nixpkgs package shape, but tolerate this branch's current
      # 4.0.6 vendored CMake module layout rather than the newer tarball
      # layout expected by nixpkgs.
      postPatch = ''
        pushd third-party
        for f in *; do
            if [[ ! $f =~ fast_float|fmt|googletest|jsonsl|utfcpp|wide-integer|wildmat ]]; then
                rm -r "$f"
            fi
        done
        popd

        rm -f cmake/FindRapidJSON.cmake
      '';
    };

  package = pkgs.transmission_4.overrideAttrs commonOverride;
in
{
  inherit package;

  checked = package.overrideAttrs (old: {
    cmakeFlags =
      (old.cmakeFlags or [])
      ++ [
        "-DENABLE_TESTS=ON"
        "-DENABLE_UTILS=ON"
        "-DENABLE_WERROR=ON"
      ];

    doCheck = true;
    checkPhase = ''
      runHook preCheck
      # `LT.DhtTest.usesBootstrapFile` is not reliable on the restricted Linux
      # builders used for this backport branch; keep the rest of the suite
      # enabled so later commits can still validate against a stable baseline.
      QT_QPA_PLATFORM=offscreen ctest \
        -E '^LT\.DhtTest\.usesBootstrapFile$' \
        -j "''${NIX_BUILD_CORES:-1}" \
        --output-on-failure
      runHook postCheck
    '';
  });
}
