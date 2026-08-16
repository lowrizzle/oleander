// A pedal which is available to be added as an active pedal.
class AvailablePedal extends React.Component {
  constructor(props) {
    super(props);
  }

  add() {
    $.get("/add_pedal/" + this.props.name);
  }

  render() {
    return (
      <button class="btn btn-outline-light btn-sm available-pedal-btn"
              onClick={this.add.bind(this)}>
        + {this.props.name}
      </button>
    )
  }
}

// The list of pedals available to be added to the board.
class AvailablePedalList extends React.Component {
  constructor(props) {
    super(props);
    this.state = {
      "pedals": [],
    };
  }

  componentDidMount() {
    $.get("/available_pedals")
      .done((pedals) => {
        this.setState({
          "pedals": pedals.sort(),
        });
      });
  }

  render() {
    const availablePedals = this.state.pedals.map((pedal) => {
      return (<AvailablePedal key={pedal} name={pedal} />)
    });

    return (
      <div class="available-pedal-list" align="center">
        {availablePedals}
      </div>
    )
  }
}

// A knob which adjusts the settings of an active pedal.
class Knob extends React.Component {
  constructor(props) {
    super(props);
  }

  tweak(sign) {
    const knobUpdate = {
      'name': this.props.name,
      'value': this.props.value + (sign * this.props.tweakAmount),
    };
    $.get('/adjust_knob/' + this.props.pedalId, knobUpdate);
  }

  onChange(event) {
    const knobUpdate = {
      'name': this.props.name,
      'value': event.target.value,
    };

    $.get('/adjust_knob/' + this.props.pedalId, knobUpdate);
  }

  render() {
    return (
      <div class="knob">
        <span class="knob-label">{this.props.name}</span>
        <button class="btn btn-dark btn-sm knob-btn"
                onClick={this.tweak.bind(this, -1)}>
          -
        </button>
        <input class="knob-input" value={this.props.value} onChange={this.onChange.bind(this)} />
        <button class="btn btn-dark btn-sm knob-btn"
                onClick={this.tweak.bind(this, 1)}>
          +
        </button>
      </div>
    )
  }
}

// An active pedal which is running on the board and applying effects to the signal.
// Pedals are addressed by their stable `id` (assigned by the server), not
// by their position in the list -- that position shifts every time any
// pedal is added or removed, so a stale index could end up hitting the
// wrong pedal if a request is in flight when the chain changes.
class ActivePedal extends React.Component {
  constructor(props) {
    super(props);
  }

  remove() {
    $.get('/remove_pedal/' + this.props.id);
  }

  push() {
    $.get('/push_button/' + this.props.id);
  }

  render() {
    const knobs = this.props.knobs.map((knob) => {
      return (
          <Knob
            key={knob.name}
            name={knob.name}
            value={knob.value}
            tweakAmount={knob.tweak_amount}
            pedalId={this.props.id} />
      )
    });

    const btnClass = this.props.state === "Enabled"
      ? "btn btn-success pedal-switch"
      : "btn btn-secondary pedal-switch";

    return (
      <div class="active-pedal-card">
        <div class="pedal-header">
          <h5 class="pedal-name">{this.props.name}</h5>
          <span class="pedal-index">#{this.props.position}</span>
        </div>

        <div class="pedal-knobs">
          {knobs}
        </div>

        <div class="pedal-actions">
          <button
            class={btnClass}
            onClick={this.push.bind(this)}>
            {this.props.state === "Enabled" ? "ON" : "OFF"}
          </button>
          &nbsp;
          <button
            class="btn btn-danger btn-sm"
            onClick={this.remove.bind(this)} >
            Remove
          </button>
        </div>
      </div>
    )
  }
}

// The board of active pedals. Refreshes whenever `updateToken` changes
// (App bumps it on every WebSocket "ping" from the server) rather than
// maintaining its own socket connection.
class PedalBoard extends React.Component {
  constructor(props) {
    super(props);
    this.state = {
      'pedals': []
    };
  }

  componentDidMount() {
    this.refresh();
  }

  componentDidUpdate(prevProps) {
    if (prevProps.updateToken !== this.props.updateToken) {
      this.refresh();
    }
  }

  refresh() {
    $.get('/active_pedals').done(response => {
      this.setState({
        'pedals': response.pedals
      });
    });
  }

  render() {
    if (this.state.pedals.length === 0) {
      return (
        <div class="splash-screen">
          <h1 class="splash-title">OLEANDER</h1>
          <p class="splash-subtitle">Multi-Effects Pedal</p>
          <p class="splash-hint">Add pedals below to get started</p>
        </div>
      );
    }

    var fullView = this.state.pedals.map((pedal, position) => {
      return (
          <div className="row pedal-row" key={pedal.id}>
            <div className="col-md-8 offset-md-2 col-xs-12">
              <ActivePedal
                id={pedal.id}
                name={pedal.name}
                state={pedal.state}
                knobs={pedal.knobs}
                position={position + 1} />
            </div>
          </div>
      )
    });

    return (
      <div>
        {fullView}
      </div>
    );
  }
}

// A single preset slot: tap to recall it onto the live board, or save the
// board's current state into this slot (optionally renaming it).
class PresetTile extends React.Component {
  constructor(props) {
    super(props);
  }

  load() {
    $.get('/preset/' + this.props.index);
  }

  save(event) {
    event.stopPropagation();
    const name = window.prompt('Save current settings as:', this.props.name);
    if (name === null) {
      return; // cancelled
    }
    // `name` travels as a query param (not a POST body) to match the
    // convention every other mutating endpoint in this app already uses.
    $.post('/preset/' + this.props.index + '/save?name=' + encodeURIComponent(name));
  }

  render() {
    const tileClass = 'preset-tile' + (this.props.active ? ' preset-tile-active' : '');
    const pedalCountLabel = this.props.pedalCount === 1
      ? '1 pedal'
      : this.props.pedalCount + ' pedals';
    // Reflects the physical footswitch's live latch position, independent
    // of whether this preset is the one currently active -- a switch can
    // be latched on while the board has since been freely edited away
    // from it, and this dot should keep tracking the switch either way.
    const switchDotClass = 'switch-status-dot' +
      (this.props.switchLatched ? ' switch-status-dot-on' : '');
    const switchDotTitle = 'Footswitch ' + (this.props.index + 1) +
      (this.props.switchLatched ? ' is latched ON' : ' is latched OFF');

    return (
      <div class={tileClass} onClick={this.load.bind(this)}>
        <div class="preset-slot">
          {this.props.index + 1}
          <span class={switchDotClass} title={switchDotTitle}></span>
        </div>
        <div class="preset-name">{this.props.name}</div>
        <div class="preset-count">{pedalCountLabel}</div>
        <button
          class="btn btn-outline-light btn-sm preset-save-btn"
          title="Save the current settings into this preset"
          onClick={this.save.bind(this)}>
          Save here
        </button>
      </div>
    )
  }
}

// The row of 5 preset slots. Mirrors the physical footswitches: tapping a
// tile does exactly what pressing the matching switch does
// (GET /preset/<n>), and the currently active slot is highlighted the
// same way whether it was selected from the browser or the pedalboard.
class PresetBar extends React.Component {
  constructor(props) {
    super(props);
    this.state = {
      'presets': [],
      'activeIndex': -1,
      // Keyed by switch/preset index (0-4) -> bool. Fetched separately
      // from /presets since it comes from a different backend store
      // (SwitchStates, not PresetStore) -- a switch's latch position and
      // "which preset is loaded" are related but distinct pieces of state.
      'switchLatched': {},
    };
  }

  componentDidMount() {
    this.refresh();
  }

  componentDidUpdate(prevProps) {
    if (prevProps.updateToken !== this.props.updateToken) {
      this.refresh();
    }
  }

  refresh() {
    $.get('/presets').done(response => {
      this.setState({
        'presets': response.presets,
        'activeIndex': response.active_index,
      });
    });
    $.get('/switches').done(response => {
      const switchLatched = {};
      response.switches.forEach(sw => {
        switchLatched[sw.index] = sw.pressed;
      });
      this.setState({ 'switchLatched': switchLatched });
    });
  }

  render() {
    const tiles = this.state.presets.map(preset => (
      <PresetTile
        key={preset.index}
        index={preset.index}
        name={preset.name}
        pedalCount={preset.pedal_count}
        active={preset.index === this.state.activeIndex}
        switchLatched={!!this.state.switchLatched[preset.index]} />
    ));

    return (
      <div class="preset-bar">
        {tiles}
      </div>
    )
  }
}

class App extends React.Component {
  constructor(props) {
    super(props);
    this.state = {
      'updateToken': 0,
    };
    this.socket = null;
    this.reconnectDelayMs = 1000;
    this.reconnectTimer = null;
  }

  componentDidMount() {
    this.connect();
  }

  componentWillUnmount() {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
    }
    if (this.socket) {
      this.socket.onclose = null; // this unmount is the intentional close
      this.socket.close();
    }
  }

  // Owns the single WebSocket connection for the whole page (both
  // PresetBar and PedalBoard refresh off of `updateToken`) and reconnects
  // with backoff if it drops -- previously there was no reconnect logic
  // at all, so a Pi reboot or a Wi-Fi blip left the page silently frozen
  // on stale state (including "which preset is active") until someone
  // manually refreshed the tab.
  connect() {
    const sock = new WebSocket("ws://" + window.location.host + "/updates");

    sock.onopen = () => {
      this.reconnectDelayMs = 1000;
      this.bumpUpdateToken(); // catch up on anything missed while down
    };
    sock.onmessage = () => {
      this.bumpUpdateToken();
    };
    sock.onclose = () => {
      this.reconnectTimer = setTimeout(() => this.connect(), this.reconnectDelayMs);
      this.reconnectDelayMs = Math.min(this.reconnectDelayMs * 2, 15000);
    };

    this.socket = sock;
  }

  bumpUpdateToken() {
    this.setState(prevState => ({ updateToken: prevState.updateToken + 1 }));
  }

  render() {
    return (
      <div class="container-fluid pedalboard-container">
        <h2 class="app-title">Oleander</h2>
        <PresetBar updateToken={this.state.updateToken} />
        <hr />
        <AvailablePedalList />
        <hr />
        <PedalBoard updateToken={this.state.updateToken} />
      </div>
    )
  }
}

// Fetch the feed from the API and render it.
$(document).ready(() => {
  ReactDOM.render(
      <App />,
      document.getElementById('pedalboard')
  );
});
