
open Analyze;
open DigTypes;

let getType = (~env: Query.queryEnv, name) => {
  open Infix;
  let%try stamp =
    Query.hashFind(env.exported.types, name)
    |> RResult.orError(
         "No exported type " ++ name ++ " in module " ++ env.file.uri ++ " " ++ (Stdlib.Hashtbl.to_seq_keys(env.exported.types)->Stdlib.List.of_seq |> String.concat(",")),
       );
  Query.hashFind(env.file.stamps.types, stamp) |> RResult.orError("Invalid stamp for " ++ name)
};

let isBuiltin = fun
  | "list" | "string" | "option" | "int" | "float" | "bool" | "array" => true
  | _ => false;

let rec getFullType_ = (env, path, name) => switch path {
  | [] => getType(~env, name)
  | [one, ...more] =>
    let%try modStamp = Query.hashFind(env.exported.modules, one) |> RResult.orError("No exported module " ++ one);
    let%try declared = Query.hashFind(env.file.stamps.modules, modStamp) |> RResult.orError("Unexpected stamp " ++ string_of_int(modStamp));
    switch (declared.contents) {
      | Ident(_) => Error("Expected a module, but " ++ one ++ " was an identifier")
      | Structure(contents) =>
        getFullType_({...env, exported: contents.exported}, more, name)
    }
};

let getFullType = (~env: Query.queryEnv, path, name) => {
  getFullType_(env, path, name)
};

let mapSource = (~env, ~getModule, path) => {
    let resolved = Query.resolveFromCompilerPath(~env, ~getModule, path);
    open Infix;
    let declared =
      switch (resolved) {
      | `Not_found =>
        print_endline("Unresolved " ++ Path.name(path) ++ " in " ++ env.file.uri);
        None
      | `Stamp(stamp) =>
        switch (Query.hashFind(env.file.stamps.types, stamp)) {
          | None =>
            switch (Path.name(path)) {
              | "list" | "string" | "option" | "int" | "float" | "bool" | "array" => None
              | _ =>
              print_endline("Bad stamp " ++ Path.name(path) ++ " in " ++ env.file.uri ++ " :: " ++ string_of_int(stamp))
              None
            }
          | Some(decl) => Some((decl, env))
        }
         /* |?>> (d => (d, env.file)) */
      | `Exported(env, name) => switch (getType(~env, name)) {
        | Error(_) => None
        | Ok(d) => Some((d, env))
      }
      };
    switch (declared) {
    | None => switch path {
      | Path.Pident(ident) when isBuiltin(Ident.name(ident)) => Builtin(Ident.name(ident))
      | _ => {
        print_endline("!!! Not found " ++ Path.name(path));
        NotFound
      }
    }
    | Some(({name, modulePath} as declared, env)) =>
      let%try_force ((uri, moduleName), path) = Query.showVisibilityPath(~env, ~getModule, modulePath) |> RResult.orError("No visibility path");
      Public({
        uri,
        moduleName,
        declared,
        modulePath: path,
        name: name.txt,
        env,
      })
    };

  };


let recursiveMapSource = (~env, ~getModule, loop, path) => {
  let result = mapSource(~env, ~getModule, path);
  switch result {
    | Public({
      moduleName,
      modulePath: path,
      declared,
      name,
      env,
    }) =>
      loop(~env, (moduleName, path, name), declared)
    | _ => ()
  };

  result
};

let rec digType = (~tbl, ~set, ~state, ~package, ~env, ~getModule, key, t: SharedTypes.declared(SharedTypes.Type.t)) => {
  if (!Hashtbl.mem(set, key)) {
    let loop = digType(~tbl, ~set, ~state, ~package, ~getModule);
    Hashtbl.replace(set, key, ());
    Hashtbl.replace(tbl, key,
        (
          t.contents.typ.migrateAttributes(),
      SharedTypes.SimpleType.declMapSource(
        recursiveMapSource(~env, ~getModule, loop),
          t.contents.typ.asSimpleDeclaration(t.name.txt),
        )
      )
    )
  };
};

let splitFull = fullName => {
  let parts = Util.Utils.split_on_char('.', fullName);
  let rec loop = parts => switch parts {
    | [] => assert(false)
    | [one] => ([], one)
    | [one, ...more] =>
      let (path, last) = loop(more);
      ([one, ...path], last)
  };
  loop(parts)
};

let fileToReference = (~state, uri, fullName) => {
  let%try package = Packages.getPackage(~reportDiagnostics=(_, _) => (), uri, state);
  let (path, name) = splitFull(fullName);
  let getModuleName = uri => {
    let%opt path = Utils.parseUri(uri);
    package.nameForPath->Query.hashFind(path);
  };
  let%try_force moduleName = getModuleName(uri) |> RResult.orError("File to reference " ++ uri);
  Ok((moduleName, path, name))
};

let forInitialType = (~tbl, ~state, uri, fullName) => {
  let%try package = Packages.getPackage(~reportDiagnostics=(_, _) => (), uri, state);
  /* print_endline("Got package..."); */
  let%try (file, _) = State.fileForUri(state, ~package, uri);
  let env = Query.fileEnv(file);
  let (path, name) = splitFull(fullName);
  let%try declared = getFullType(~env, path, name);
  let getModule = State.fileForModule(state, ~package);
  // For debugging module resolution
  // let getModule = path => {
  //   print_endline("Finding file for module " ++ path);
  //   Util.Log.spamError := true;
  //   let res = State.fileForModule(state, ~package, path);
  //   Util.Log.spamError := false;
  //   res
  // };
  let getModuleName = uri => {
    let%opt path = Utils.parseUri(uri);
    package.nameForPath->Query.hashFind(path);
  };
  let moduleName = switch (getModuleName(uri)) {
    | None =>
      print_endline("Paths");
      package.nameForPath |> Hashtbl.iter((k, v) => {
        print_endline(k);
      });
      package.pathsForModule |> Hashtbl.iter((moduleName, paths) => {
        print_endline(moduleName)
      });
      raise(Failure("Entry file " ++ uri ++ " not processed / found"))
    | Some(moduleName) => moduleName
  };
  // let%try_force moduleName = getModuleName(uri) |> RResult.orError("For initial type, no module name " ++ uri);
  let set = Hashtbl.create(10);
  Hashtbl.iter((k, _) => Hashtbl.replace(set, k, ()), tbl);
  ignore(
    digType(
      ~tbl,
      ~set,
      ~state,
      ~package,
      ~env,
      ~getModule,
      (moduleName, path, name),
      declared,
    ),
  );

  Ok((moduleName, path, name));
};

let toSimpleMap = (tbl) => {
  let ntbl = Hashtbl.create(10);
  Hashtbl.iter((key, (attributes, value)) => {
    Hashtbl.replace(ntbl, key, (attributes, SharedTypes.SimpleType.declMapSource(DigTypes.toShortSource, value)))
  }, tbl);
  ntbl
};
