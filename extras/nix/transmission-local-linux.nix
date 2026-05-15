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
      # CMake module layout rather than the released 4.1.1 tarball layout.
      postPatch = ''
        pushd third-party
        for f in *; do
            if [[ ! $f =~ googletest|wildmat|wide-integer|jsonsl|madler-crcany|sigslot ]]; then
                rm -r "$f"
            fi
        done
        popd

        rm -f \
          cmake/FindRapidJSON.cmake \
          cmake/Findfmt.cmake \
          cmake/Findutf8cpp.cmake
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
      QT_QPA_PLATFORM=offscreen ctest -j "''${NIX_BUILD_CORES:-1}" --output-on-failure
      runHook postCheck
    '';
  });
}
