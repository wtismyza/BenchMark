"""Python Enumerations"""

import sys
from collections import OrderedDict
from types import MappingProxyType

__all__ = ['Enum', 'IntEnum']


class _RouteClassAttributeToGetattr:
    """Route attribute access on a class to __getattr__.

    This is a descriptor, used to define attributes that act differently when
    accessed through an instance and through a class.  Instance access remains
    normal, but access to an attribute through a class will be routed to the
    class's __getattr__ method; this is done by raising AttributeError.

    """
    def __init__(self, fget=None):
        self.fget = fget

    def __get__(self, instance, ownerclass=None):
        if instance is None:
            raise AttributeError()
        return self.fget(instance)

    def __set__(self, instance, value):
        raise AttributeError("can't set attribute")

    def __delete__(self, instance):
        raise AttributeError("can't delete attribute")


def _is_dunder(name):
    """Returns True if a __dunder__ name, False otherwise."""
    return (name[:2] == name[-2:] == '__' and
            name[2:3] != '_' and
            name[-3:-2] != '_')


def _is_sunder(name):
    """Returns True if a _sunder_ name, False otherwise."""
    return (name[0] == name[-1] == '_' and
            name[1:2] != '_' and
            name[-2:-1] != '_')


def _make_class_unpicklable(cls):
    """Make the given class un-picklable."""
    def _break_on_call_reduce(self):
        raise TypeError('%r cannot be pickled' % self)
    cls.__reduce__ = _break_on_call_reduce
    cls.__module__ = '<unknown>'


class _EnumDict(dict):
    """Keeps track of definition order of the enum items.

    EnumMeta will use the names found in self._member_names as the
    enumeration member names.

    """
    def __init__(self):
        super().__init__()
        self._member_names = []

    def __setitem__(self, key, value):
        """Changes anything not dundered or that doesn't have __get__.

        If a descriptor is added with the same name as an enum member, the name
        is removed from _member_names (this may leave a hole in the numerical
        sequence of values).

        If an enum member name is used twice, an error is raised; duplicate
        values are not checked for.

        Single underscore (sunder) names are reserved.

        """
        if _is_sunder(key):
            raise ValueError('_names_ are reserved for future Enum use')
        elif _is_dunder(key) or hasattr(value, '__get__'):
            if key in self._member_names:
                # overwriting an enum with a method?  then remove the name from
                # _member_names or it will become an enum anyway when the class
                # is created
                self._member_names.remove(key)
        else:
            if key in self._member_names:
                raise TypeError('Attempted to reuse key: %r' % key)
            self._member_names.append(key)
        super().__setitem__(key, value)


# Dummy value for Enum as EnumMeta explicity checks for it, but of course until
# EnumMeta finishes running the first time the Enum class doesn't exist.  This
# is also why there are checks in EnumMeta like `if Enum is not None`
Enum = None


class EnumMeta(type):
    """Metaclass for Enum"""
    @classmethod
    def __prepare__(metacls, cls, bases):
        return _EnumDict()

    def __new__(metacls, cls, bases, classdict):
        # an Enum class is final once enumeration items have been defined; it
        # cannot be mixed with other types (int, float, etc.) if it has an
        # inherited __new__ unless a new __new__ is defined (or the resulting
        # class will fail).
        member_type, first_enum = metacls._get_mixins_(bases)
        __new__, save_new, use_args = metacls._find_new_(classdict, member_type,
                                                        first_enum)

        # save enum items into separate mapping so they don't get baked into
        # the new class
        members = {k: classdict[k] for k in classdict._member_names}
        for name in classdict._member_names:
            del classdict[name]

        # check for illegal enum names (any others?)
        invalid_names = set(members) & {'mro', }
        if invalid_names:
            raise ValueError('Invalid enum member name: {0}'.format(
                ','.join(invalid_names)))

        # create our new Enum type
        enum_class = super().__new__(metacls, cls, bases, classdict)
        enum_class._member_names = []               # names in definition order
        enum_class._member_map = OrderedDict()      # name->value map

        # Reverse value->name map for hashable values.
        enum_class._value2member_map = {}

        # check for a __getnewargs__, and if not present sabotage
        # pickling, since it won't work anyway
        if (member_type is not object and
            member_type.__dict__.get('__getnewargs__') is None
            ):
            _make_class_unpicklable(enum_class)

        # instantiate them, checking for duplicates as we go
        # we instantiate first instead of checking for duplicates first in case
        # a custom __new__ is doing something funky with the values -- such as
        # auto-numbering ;)
        for member_name in classdict._member_names:
            value = members[member_name]
            if not isinstance(value, tuple):
                args = (value, )
            else:
                args = value
            if member_type is tuple:   # special case for tuple enums
                args = (args, )     # wrap it one more time
            if not use_args:
                enum_member = __new__(enum_class)
                enum_member._value = value
            else:
                enum_member = __new__(enum_class, *args)
                if not hasattr(enum_member, '_value'):
                    enum_member._value = member_type(*args)
            enum_member._member_type = member_type
            enum_member._name = member_name
            enum_member.__init__(*args)
            # If another member with the same value was already defined, the
            # new member becomes an alias to the existing one.
            for name, canonical_member in enum_class._member_map.items():
                if canonical_member.value == enum_member._value:
                    enum_member = canonical_member
                    break
            else:
                # Aliases don't appear in member names (only in __members__).
                enum_class._member_names.append(member_name)
            enum_class._member_map[member_name] = enum_member
            try:
                # This may fail if value is not hashable. We can't add the value
                # to the map, and by-value lookups for this value will be
                # linear.
                enum_class._value2member_map[value] = enum_member
            except TypeError:
                pass

        # double check that repr and friends are not the mixin's or various
        # things break (such as pickle)
        for name in ('__repr__', '__str__', '__getnewargs__'):
            class_method = getattr(enum_class, name)
            obj_method = getattr(member_type, name, None)
            enum_method = getattr(first_enum, name, None)
            if obj_method is not None and obj_method is class_method:
                setattr(enum_class, name, enum_method)

        # replace any other __new__ with our own (as long as Enum is not None,
        # anyway) -- again, this is to support pickle
        if Enum is not None:
            # if the user defined their own __new__, save it before it gets
            # clobbered in case they subclass later
            if save_new:
                enum_class.__new_member__ = __new__
            enum_class.__new__ = Enum.__new__
        return enum_class

    def __call__(cls, value, names=None, *, module=None, type=None):
        """Either returns an existing member, or creates a new enum class.

        This method is used both when an enum class is given a value to match
        to an enumeration member (i.e. Color(3)) and for the functional API
        (i.e. Color = Enum('Color', names='red green blue')).

        When used for the functional API: `module`, if set, will be stored in
        the new class' __module__ attribute; `type`, if set, will be mixed in
        as the first base class.

        Note: if `module` is not set this routine will attempt to discover the
        calling module by walking the frame stack; if this is unsuccessful
        the resulting class will not be pickleable.

        """
        if names is None:  # simple value lookup
            return cls.__new__(cls, value)
        # otherwise, functional API: we're creating a new Enum type
        return cls._create_(value, names, module=module, type=type)

    def __contains__(cls, member):
        return isinstance(member, cls) and member.name in cls._member_map

    def __dir__(self):
        return ['__class__', '__doc__', '__members__'] + self._member_names

    @property
    def __members__(cls):
        """Returns a mapping of member name->value.

        This mapping lists all enum members, including aliases. Note that this
        is a read-only view of the internal mapping.

        """
        return MappingProxyType(cls._member_map)

    def __getattr__(cls, name):
        """Return the enum member matching `name`

        We use __getattr__ instead of descriptors or inserting into the enum
        class' __dict__ in order to support `name` and `value` being both
        properties for enum members (which live in the class' __dict__) and
        enum members themselves.

        """
        if _is_dunder(name):
            raise AttributeError(name)
        try:
            return cls._member_map[name]
        except KeyError:
            raise AttributeError(name) from None

    def __getitem__(cls, name):
        return cls._member_map[name]

    def __iter__(cls):
        return (cls._member_map[name] for name in cls._member_names)

    def __len__(cls):
        return len(cls._member_names)

    def __repr__(cls):
        return "<enum %r>" % cls.__name__

    def _create_(cls, class_name, names=None, *, module=None, type=None):
        """Convenience method to create a new Enum class.

        `names` can be:

        * A string containing member names, separated either with spaces or
          commas.  Values are auto-numbered from 1.
        * An iterable of member names.  Values are auto-numbered from 1.
        * An iterable of (member name, value) pairs.
        * A mapping of member name -> value.

        """
        metacls = cls.__class__
        bases = (cls, ) if type is None else (type, cls)
        classdict = metacls.__prepare__(class_name, bases)

        # special processing needed for names?
        if isinstance(names, str):
            names = names.replace(',', ' ').split()
        if isinstance(names, (tuple, list)) and isinstance(names[0], str):
            names = [(e, i) for (i, e) in enumerate(names, 1)]

        # Here, names is either an iterable of (name, value) or a mapping.
        for item in names:
            if isinstance(item, str):
                member_name, member_value = item, names[item]
            else:
                member_name, member_value = item
            classdict[member_name] = member_value
        enum_class = metacls.__new__(metacls, class_name, bases, classdict)

        # TODO: replace the frame hack if a blessed way to know the calling
        # module is ever developed
        if module is None:
            try:
                module = sys._getframe(2).f_globals['__name__']
            except (AttributeError, ValueError) as exc:
                pass
        if module is None:
            _make_class_unpicklable(enum_class)
        else:
            enum_class.__module__ = module

        return enum_class

    @staticmethod
    def _get_mixins_(bases):
        """Returns the type for creating enum members, and the first inherited
        enum class.

        bases: the tuple of bases that was given to __new__

        """
        if not bases:
            return object, Enum

        # double check that we are not subclassing a class with existing
        # enumeration members; while we're at it, see if any other data
        # type has been mixed in so we can use the correct __new__
        member_type = first_enum = None
        for base in bases:
            if  (base is not Enum and
                    issubclass(base, Enum) and
                    base._member_names):
                raise TypeError("Cannot extend enumerations")
        # base is now the last base in bases
        if not issubclass(base, Enum):
            raise TypeError("new enumerations must be created as "
                    "`ClassName([mixin_type,] enum_type)`")

        # get correct mix-in type (either mix-in type of Enum subclass, or
        # first base if last base is Enum)
        if not issubclass(bases[0], Enum):
            member_type = bases[0]     # first data type
            first_enum = bases[-1]  # enum type
        else:
            for base in bases[0].__mro__:
                # most common: (IntEnum, int, Enum, object)
                # possible:    (<Enum 'AutoIntEnum'>, <Enum 'IntEnum'>,
                #               <class 'int'>, <Enum 'Enum'>,
                #               <class 'object'>)
                if issubclass(base, Enum):
                    if first_enum is None:
                        first_enum = base
                else:
                    if member_type is None:
                        member_type = base

        return member_type, first_enum

    @staticmethod
    def _find_new_(classdict, member_type, first_enum):
        """Returns the __new__ to be used for creating the enum members.

        classdict: the class dictionary given to __new__
        member_type: the data type whose __new__ will be used by default
        first_enum: enumeration to check for an overriding __new__

        """
        # now find the correct __new__, checking to see of one was defined
        # by the user; also check earlier enum classes in case a __new__ was
        # saved as __new_member__
        __new__ = classdict.get('__new__', None)

        # should __new__ be saved as __new_member__ later?
        save_new = __new__ is not None

        if __new__ is None:
            # check all possibles for __new_member__ before falling back to
            # __new__
            for method in ('__new_member__', '__new__'):
                for possible in (member_type, first_enum):
                    target = getattr(possible, method, None)
                    if target not in {
                            None,
                            None.__new__,
                            object.__new__,
                            Enum.__new__,
                            }:
                        __new__ = target
                        break
                if __new__ is not None:
                    break
            else:
                __new__ = object.__new__

        # if a non-object.__new__ is used then whatever value/tuple was
        # assigned to the enum member name will be passed to __new__ and to the
        # new enum member's __init__
        if __new__ is object.__new__:
            use_args = False
        else:
            use_args = True

        return __new__, save_new, use_args


class Enum(metaclass=EnumMeta):
    """Generic enumeration.

    Derive from this class to define new enumerations.

    """
    def __new__(cls, value):
        # all enum instances are actually created during class construction
        # without calling this method; this method is called by the metaclass'
        # __call__ (i.e. Color(3) ), and by pickle
        if type(value) is cls:
            # For lookups like Color(Color.red)
            return value
        # by-value search for a matching enum member
        # see if it's in the reverse mapping (for hashable values)
        if value in cls._value2member_map:
            return cls._value2member_map[value]
        # not there, now do long search -- O(n) behavior
        for member in cls._member_map.values():
            if member.value == value:
                return member
        raise ValueError("%s is not a valid %s" % (value, cls.__name__))

    def __repr__(self):
        return "<%s.%s: %r>" % (
                self.__class__.__name__, self._name, self._value)

    def __str__(self):
        return "%s.%s" % (self.__class__.__name__, self._name)

    def __dir__(self):
        return (['__class__', '__doc__', 'name', 'value'])

    def __eq__(self, other):
        if type(other) is self.__class__:
            return self is other
        return NotImplemented

    def __getnewargs__(self):
        return (self._value, )

    def __hash__(self):
        return hash(self._name)

    # _RouteClassAttributeToGetattr is used to provide access to the `name`
    # and `value` properties of enum members while keeping some measure of
    # protection from modification, while still allowing for an enumeration
    # to have members named `name` and `value`.  This works because enumeration
    # members are not set directly on the enum class -- __getattr__ is
    # used to look them up.

    @_RouteClassAttributeToGetattr
    def name(self):
        return self._name

    @_RouteClassAttributeToGetattr
    def value(self):
        return self._value


class IntEnum(int, Enum):
    """Enum where members are also (and must be) ints"""
